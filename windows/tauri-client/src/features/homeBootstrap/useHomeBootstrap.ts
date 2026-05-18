import { nextTick, onMounted, ref } from "vue";
import type { HomeDiskListItem, HomeDiskListSnapshot } from "../../entities/disk/model";
import type { AppSessionPhase, AppSessionSnapshot } from "../../entities/appSession/model";
import {
  mountDisk,
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
  const errorText = ref<string | null>(null);
  const appSessionPhase = ref<AppSessionPhase>("initializing");
  const appSessionStatusText = ref<string | null>("正在恢复配置");
  const diskDisplayPhase = ref<HomeDiskDisplayPhase>("startup");
  const appSessionSnapshot = ref<AppSessionSnapshot | null>(null);
  const initialAutoMountCompleted = ref(false);
  const actionLoadingDiskId = ref<string | null>(null);

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

  async function loadHomeDiskList(options: { showLoading?: boolean } = {}): Promise<HomeDiskListSnapshot | null> {
    return runHomeDiskListOperation(queryHomeDiskList, options);
  }

  async function handleRescanRuntimeDisks(
    options: { showLoading?: boolean } = {},
  ): Promise<HomeDiskListSnapshot | null> {
    return runHomeDiskListOperation(rescanRuntimeDisks, {
      ...options,
      preserveSnapshotOnError: true,
    });
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

  async function runOpenAppSessionFlow(): Promise<boolean> {
    errorText.value = null;
    appSessionPhase.value = "initializing";
    diskDisplayPhase.value = "startup";
    appSessionStatusText.value = "正在打开 Backend 会话";

    try {
      appSessionSnapshot.value = await openAppSession();
      appSessionPhase.value = "ready";

      appSessionStatusText.value = "正在重扫磁盘运行态";
      const rescanSnapshot = await handleRescanRuntimeDisks({ showLoading: false });
      if (rescanSnapshot === null) {
        setAppSessionFailureState(errorText.value ?? "重扫磁盘运行态失败");
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
    void bootstrapHomePage();
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
    retryOpenAppSessionFlow,
    runtimeDisks,
    appSessionPhase,
    appSessionSnapshot,
    appSessionStatusText,
  };
}
