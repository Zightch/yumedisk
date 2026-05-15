import { nextTick, onMounted, ref } from "vue";
import type { HomeDiskListItem, HomeDiskListSnapshot } from "../../entities/disk/model";
import type { SessionPhase, SessionSnapshot } from "../../entities/session/model";
import {
  mountDisk,
  queryHomeDiskList,
  rescanRuntimeDisks,
} from "../../shared/api/diskClient";
import {
  getErrorDetail,
  getErrorMessage,
  openSession,
  restoreClientState,
} from "../../shared/api/sessionClient";
import type { HomeDiskDisplayPhase } from "./homeDisplayMapper";

export interface HomeBootstrapState {
  runtimeDisks: HomeDiskListItem[];
  autoMountCount: number;
  loading: boolean;
  errorText: string | null;
  sessionPhase: SessionPhase;
  sessionStatusText: string | null;
  diskDisplayPhase: HomeDiskDisplayPhase;
  sessionSnapshot: SessionSnapshot | null;
}

export function useHomeBootstrap() {
  const runtimeDisks = ref<HomeDiskListItem[]>([]);
  const autoMountCount = ref(0);
  const loading = ref(true);
  const errorText = ref<string | null>(null);
  const sessionPhase = ref<SessionPhase>("initializing");
  const sessionStatusText = ref<string | null>("正在恢复配置");
  const diskDisplayPhase = ref<HomeDiskDisplayPhase>("startup");
  const sessionSnapshot = ref<SessionSnapshot | null>(null);
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
    diskId: string,
    options: { silentSuccess?: boolean } = {},
  ): Promise<{ ok: boolean; errorText: string | null }> {
    actionLoadingDiskId.value = diskId;

    try {
      await mountDisk({ diskId });
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

    const diskIds = snapshot.disks
      .filter((disk) => disk.autoMount && disk.status === "unmounted")
      .map((disk) => disk.diskId);

    for (const diskId of diskIds) {
      await handleMountDisk(diskId, { silentSuccess: true });
    }
  }

  function setSessionFailureState(text: string): void {
    sessionPhase.value = "failed";
    sessionStatusText.value = text;
    diskDisplayPhase.value = "startup";
  }

  function resolveSessionFailureText(error: unknown): string {
    return getErrorDetail(error) ?? getErrorMessage(error);
  }

  async function runOpenSessionFlow(): Promise<boolean> {
    errorText.value = null;
    sessionPhase.value = "initializing";
    diskDisplayPhase.value = "startup";
    sessionStatusText.value = "正在打开 Backend 会话";

    try {
      sessionSnapshot.value = await openSession();
      sessionPhase.value = "ready";

      sessionStatusText.value = "正在重扫磁盘运行态";
      const rescanSnapshot = await handleRescanRuntimeDisks({ showLoading: false });
      if (rescanSnapshot === null) {
        setSessionFailureState(errorText.value ?? "重扫磁盘运行态失败");
        return false;
      }

      diskDisplayPhase.value = "normal";
      await nextTick();

      sessionStatusText.value = "正在执行自动挂载";
      await runInitialAutoMount(rescanSnapshot);
      sessionStatusText.value = sessionSnapshot.value.stateText;
      return true;
    } catch (error) {
      sessionSnapshot.value = null;
      setSessionFailureState(resolveSessionFailureText(error));
      errorText.value = getErrorMessage(error);
      return false;
    }
  }

  async function bootstrapHomePage() {
    loading.value = true;
    errorText.value = null;
    actionLoadingDiskId.value = null;
    sessionSnapshot.value = null;
    sessionPhase.value = "initializing";
    sessionStatusText.value = "正在恢复配置";
    diskDisplayPhase.value = "startup";
    initialAutoMountCompleted.value = false;

    try {
      await restoreClientState();

      sessionStatusText.value = "正在加载磁盘配置";
      const snapshot = await loadHomeDiskList({ showLoading: false });
      if (snapshot === null) {
        setSessionFailureState(errorText.value ?? "加载磁盘配置失败");
        loading.value = false;
        return;
      }

      loading.value = false;
      await nextTick();

      await runOpenSessionFlow();
    } catch (error) {
      sessionSnapshot.value = null;
      setSessionFailureState(resolveSessionFailureText(error));
      errorText.value = getErrorMessage(error);
    } finally {
      loading.value = false;
    }
  }

  async function retryOpenSessionFlow(): Promise<boolean> {
    initialAutoMountCompleted.value = false;
    actionLoadingDiskId.value = null;
    return runOpenSessionFlow();
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
    retryOpenSessionFlow,
    runtimeDisks,
    sessionPhase,
    sessionSnapshot,
    sessionStatusText,
  };
}
