import { listen } from "@tauri-apps/api/event";
import { nextTick, onMounted, onUnmounted, ref } from "vue";
import type {
  HomeDiskListItem,
  HomeDiskListSnapshot,
  RuntimeRescanLifecycleEvent,
} from "../../entities/disk/model";
import type {
  AppSessionPhase,
  AppSessionRuntimeEvent,
  AppSessionSnapshot,
} from "../../entities/appSession/model";
import {
  mountDisk,
  queryRuntimeRescanState,
  queryHomeDiskList,
  rescanRuntimeDisks,
} from "../../shared/api/diskClient";
import {
  getErrorDetail,
  getErrorMessage,
  openAppSession,
  restoreClientState,
} from "../../shared/api/appSessionClient";
import type { HomeDiskDisplayPhase } from "./homeDisplayMapper";

export interface HomeBootstrapState {
  runtimeDisks: HomeDiskListItem[];
  autoMountCount: number;
  loading: boolean;
  rescanLoading: boolean;
  errorText: string | null;
  appSessionPhase: AppSessionPhase;
  appSessionStatusText: string | null;
  diskDisplayPhase: HomeDiskDisplayPhase;
  appSessionSnapshot: AppSessionSnapshot | null;
}

export function useHomeBootstrap() {
  const runtimeDisks = ref<HomeDiskListItem[]>([]);
  const autoMountCount = ref(0);
  const loading = ref(true);
  const rescanLoading = ref(false);
  const errorText = ref<string | null>(null);
  const appSessionPhase = ref<AppSessionPhase>("initializing");
  const appSessionStatusText = ref<string | null>("正在恢复配置");
  const diskDisplayPhase = ref<HomeDiskDisplayPhase>("startup");
  const appSessionSnapshot = ref<AppSessionSnapshot | null>(null);
  const initialAutoMountCompleted = ref(false);
  const actionLoadingDiskId = ref<string | null>(null);
  let stopNetworkRuntimeListener: (() => void) | null = null;
  let stopAppSessionRuntimeListener: (() => void) | null = null;
  let stopRuntimeRescanListener: (() => void) | null = null;
  let rescanStatePollTimer: number | null = null;
  let rescanStateSyncInFlight = false;
  let rescanWaiters: Array<{
    resolve: () => void;
    reject: (error: Error) => void;
  }> = [];

  function applyHomeDiskListSnapshot(snapshot: HomeDiskListSnapshot): void {
    runtimeDisks.value = snapshot.disks;
    autoMountCount.value = snapshot.autoMountCount;
  }

  async function runHomeDiskListOperation(
    operation: () => Promise<HomeDiskListSnapshot>,
    options: { showLoading?: boolean; preserveSnapshotOnError?: boolean } = {},
  ): Promise<HomeDiskListSnapshot | null> {
    const showLoading = options.showLoading ?? true;
    const preserveSnapshotOnError = options.preserveSnapshotOnError ?? false;
    if (showLoading) {
      loading.value = true;
    }

    errorText.value = null;

    try {
      const snapshot = await operation();
      applyHomeDiskListSnapshot(snapshot);
      return snapshot;
    } catch (error) {
      if (!preserveSnapshotOnError) {
        runtimeDisks.value = [];
        autoMountCount.value = 0;
      }
      errorText.value = getErrorMessage(error);
      return null;
    } finally {
      if (showLoading) {
        loading.value = false;
      }
    }
  }

  async function loadHomeDiskList(
    options: { showLoading?: boolean; preserveSnapshotOnError?: boolean } = {},
  ): Promise<HomeDiskListSnapshot | null> {
    return runHomeDiskListOperation(queryHomeDiskList, options);
  }

  async function handleRescanRuntimeDisks(
    options: { awaitCompletion?: boolean } = {},
  ): Promise<HomeDiskListSnapshot | null> {
    errorText.value = null;

    try {
      const result = await rescanRuntimeDisks();
      rescanLoading.value = result.running;
      ensureRescanStatePolling();

      if (options.awaitCompletion !== false && result.running) {
        await waitForActiveRescan();
      }

      return loadHomeDiskList({
        showLoading: false,
        preserveSnapshotOnError: true,
      });
    } catch (error) {
      errorText.value = getErrorMessage(error);
      return null;
    }
  }

  async function handleMountDisk(
    localDiskId: string,
    options: { silentSuccess?: boolean } = {},
  ): Promise<{ ok: boolean; errorText: string | null }> {
    actionLoadingDiskId.value = localDiskId;

    try {
      await mountDisk({ localDiskId });
      await loadHomeDiskList({ showLoading: false });
      return { ok: true, errorText: null };
    } catch (error) {
      return {
        ok: false,
        errorText: options.silentSuccess
          ? `自动挂载失败：${getErrorMessage(error)}`
          : getErrorMessage(error),
      };
    } finally {
      actionLoadingDiskId.value = null;
    }
  }

  async function runInitialAutoMount(snapshot: HomeDiskListSnapshot) {
    if (initialAutoMountCompleted.value) {
      return;
    }

    initialAutoMountCompleted.value = true;

    const localDiskIds = snapshot.disks
      .filter((disk) => disk.autoMount && disk.status === "unmounted")
      .map((disk) => disk.localDiskId);

    for (const localDiskId of localDiskIds) {
      await handleMountDisk(localDiskId, { silentSuccess: true });
    }
  }

  function setAppSessionFailureState(text: string): void {
    appSessionPhase.value = "failed";
    appSessionStatusText.value = text;
    diskDisplayPhase.value = "startup";
  }

  function resolveAppSessionFailureText(error: unknown): string {
    return getErrorDetail(error) ?? getErrorMessage(error);
  }

  function resolveRescanFailureText(errorTextValue: string | null): string {
    return errorTextValue ?? "重扫磁盘运行态失败";
  }

  function clearRescanStatePolling(): void {
    if (rescanStatePollTimer === null) {
      return;
    }

    window.clearInterval(rescanStatePollTimer);
    rescanStatePollTimer = null;
  }

  function settleRescanWaiters(errorTextValue: string | null = null) {
    clearRescanStatePolling();

    const waiters = rescanWaiters;
    rescanWaiters = [];

    if (errorTextValue === null) {
      waiters.forEach((waiter) => waiter.resolve());
      return;
    }

    waiters.forEach((waiter) => waiter.reject(new Error(errorTextValue)));
  }

  function scheduleRescanWaiterFallbackResolve(): void {
    window.setTimeout(() => {
      if (rescanLoading.value) {
        return;
      }

      settleRescanWaiters();
    }, 200);
  }

  async function syncRuntimeRescanState(): Promise<void> {
    if (rescanStateSyncInFlight) {
      return;
    }

    rescanStateSyncInFlight = true;

    try {
      const snapshot = await queryRuntimeRescanState();
      rescanLoading.value = snapshot.running;
      if (!snapshot.running) {
        clearRescanStatePolling();
        scheduleRescanWaiterFallbackResolve();
      }
    } finally {
      rescanStateSyncInFlight = false;
    }
  }

  function ensureRescanStatePolling(): void {
    if (!rescanLoading.value || rescanStatePollTimer !== null) {
      return;
    }

    rescanStatePollTimer = window.setInterval(() => {
      void syncRuntimeRescanState();
    }, 500);
  }

  function handleRuntimeRescanLifecycleEvent(payload: RuntimeRescanLifecycleEvent) {
    if (payload.phase === "started") {
      rescanLoading.value = true;
      ensureRescanStatePolling();
      return;
    }

    rescanLoading.value = false;

    if (payload.phase === "failed") {
      errorText.value = resolveRescanFailureText(payload.errorText);
      settleRescanWaiters(errorText.value);
      return;
    }

    settleRescanWaiters();
  }

  function handleAppSessionRuntimeEvent(payload: AppSessionRuntimeEvent) {
    appSessionSnapshot.value = null;
    appSessionPhase.value = payload.phase;
    appSessionStatusText.value = payload.statusText;
    diskDisplayPhase.value = "startup";
  }

  function waitForActiveRescan(): Promise<void> {
    if (!rescanLoading.value) {
      return Promise.resolve();
    }

    return new Promise<void>((resolve, reject) => {
      rescanWaiters.push({ resolve, reject });
    });
  }

  async function runOpenAppSessionFlow(): Promise<boolean> {
    errorText.value = null;
    appSessionPhase.value = "initializing";
    diskDisplayPhase.value = "startup";
    appSessionStatusText.value = "正在打开 Backend 会话";

    try {
      appSessionSnapshot.value = await openAppSession();
      appSessionPhase.value = "ready";

      appSessionStatusText.value = "正在重扫磁盘运行态";
      const rescanSnapshot = await handleRescanRuntimeDisks();
      if (rescanSnapshot === null) {
        setAppSessionFailureState(resolveRescanFailureText(errorText.value));
        return false;
      }

      diskDisplayPhase.value = "normal";
      await nextTick();

      appSessionStatusText.value = "正在执行自动挂载";
      await runInitialAutoMount(rescanSnapshot);
      appSessionStatusText.value = appSessionSnapshot.value.stateText;
      return true;
    } catch (error) {
      appSessionSnapshot.value = null;
      setAppSessionFailureState(resolveAppSessionFailureText(error));
      errorText.value = getErrorMessage(error);
      return false;
    }
  }

  async function bootstrapHomePage() {
    loading.value = true;
    errorText.value = null;
    actionLoadingDiskId.value = null;
    appSessionSnapshot.value = null;
    appSessionPhase.value = "initializing";
    appSessionStatusText.value = "正在恢复配置";
    diskDisplayPhase.value = "startup";
    initialAutoMountCompleted.value = false;

    try {
      await restoreClientState();

      appSessionStatusText.value = "正在加载磁盘配置";
      const snapshot = await loadHomeDiskList({ showLoading: false });
      if (snapshot === null) {
        setAppSessionFailureState(errorText.value ?? "加载磁盘配置失败");
        loading.value = false;
        return;
      }

      loading.value = false;
      await nextTick();

      await runOpenAppSessionFlow();
    } catch (error) {
      appSessionSnapshot.value = null;
      setAppSessionFailureState(resolveAppSessionFailureText(error));
      errorText.value = getErrorMessage(error);
    } finally {
      loading.value = false;
    }
  }

  async function retryOpenAppSessionFlow(): Promise<boolean> {
    initialAutoMountCompleted.value = false;
    actionLoadingDiskId.value = null;
    return runOpenAppSessionFlow();
  }

  onMounted(() => {
    void (async () => {
      stopNetworkRuntimeListener = await listen("network-runtime-changed", () => {
        void loadHomeDiskList({
          showLoading: false,
          preserveSnapshotOnError: true,
        });
      });
      stopAppSessionRuntimeListener = await listen<AppSessionRuntimeEvent>(
        "app-session-runtime-changed",
        (event) => {
          handleAppSessionRuntimeEvent(event.payload);
        },
      );
      stopRuntimeRescanListener = await listen<RuntimeRescanLifecycleEvent>(
        "runtime-rescan-state-changed",
        (event) => {
          handleRuntimeRescanLifecycleEvent(event.payload);
        },
      );
      const snapshot = await queryRuntimeRescanState();
      rescanLoading.value = snapshot.running;
      ensureRescanStatePolling();
      await bootstrapHomePage();
    })();
  });

  onUnmounted(() => {
    stopNetworkRuntimeListener?.();
    stopNetworkRuntimeListener = null;
    stopAppSessionRuntimeListener?.();
    stopAppSessionRuntimeListener = null;
    stopRuntimeRescanListener?.();
    stopRuntimeRescanListener = null;
    clearRescanStatePolling();
    settleRescanWaiters("页面已关闭");
  });

  return {
    actionLoadingDiskId,
    autoMountCount,
    diskDisplayPhase,
    errorText,
    handleMountDisk,
    handleRescanRuntimeDisks,
    loadHomeDiskList,
    loading,
    rescanLoading,
    retryOpenAppSessionFlow,
    runtimeDisks,
    appSessionPhase,
    appSessionSnapshot,
    appSessionStatusText,
  };
}
