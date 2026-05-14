import { nextTick, onMounted, ref } from "vue";
import type { HomeDiskListItem, HomeDiskListSnapshot } from "../../entities/disk/model";
import type { SessionPhase, SessionSnapshot } from "../../entities/session/model";
import {
  connectDisk,
  queryHomeDiskList,
  rescanRuntimeDisks,
} from "../../shared/api/diskClient";
import {
  getErrorMessage,
  openSession,
  restoreClientState,
} from "../../shared/api/sessionClient";
import type { HomeDiskDisplayPhase } from "./homeDisplayMapper";

export interface HomeBootstrapState {
  runtimeDisks: HomeDiskListItem[];
  autoConnectCount: number;
  loading: boolean;
  errorText: string | null;
  sessionPhase: SessionPhase;
  diskDisplayPhase: HomeDiskDisplayPhase;
  sessionSnapshot: SessionSnapshot | null;
}

export function useHomeBootstrap() {
  const runtimeDisks = ref<HomeDiskListItem[]>([]);
  const autoConnectCount = ref(0);
  const loading = ref(true);
  const errorText = ref<string | null>(null);
  const sessionPhase = ref<SessionPhase>("initializing");
  const diskDisplayPhase = ref<HomeDiskDisplayPhase>("startup");
  const sessionSnapshot = ref<SessionSnapshot | null>(null);
  const initialAutoConnectCompleted = ref(false);
  const actionLoadingDiskId = ref<string | null>(null);

  function applyHomeDiskListSnapshot(snapshot: HomeDiskListSnapshot): void {
    runtimeDisks.value = snapshot.disks;
    autoConnectCount.value = snapshot.autoConnectCount;
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
        autoConnectCount.value = 0;
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

  async function handleConnectDisk(
    diskId: string,
    options: { silentSuccess?: boolean } = {},
  ): Promise<{ ok: boolean; errorText: string | null }> {
    actionLoadingDiskId.value = diskId;

    try {
      await connectDisk({ diskId });
      await loadHomeDiskList({ showLoading: false });
      return { ok: true, errorText: null };
    } catch (error) {
      return {
        ok: false,
        errorText: options.silentSuccess
          ? `自动连接失败：${getErrorMessage(error)}`
          : getErrorMessage(error),
      };
    } finally {
      actionLoadingDiskId.value = null;
    }
  }

  async function runInitialAutoConnect(snapshot: HomeDiskListSnapshot) {
    if (initialAutoConnectCompleted.value) {
      return;
    }

    initialAutoConnectCompleted.value = true;

    const diskIds = snapshot.disks
      .filter((disk) => disk.autoConnect && disk.status === "disconnected")
      .map((disk) => disk.diskId);

    for (const diskId of diskIds) {
      await handleConnectDisk(diskId, { silentSuccess: true });
    }
  }

  async function bootstrapHomePage() {
    loading.value = true;
    errorText.value = null;
    actionLoadingDiskId.value = null;
    sessionSnapshot.value = null;
    sessionPhase.value = "initializing";
    diskDisplayPhase.value = "startup";
    initialAutoConnectCompleted.value = false;

    try {
      await restoreClientState();

      const snapshot = await loadHomeDiskList({ showLoading: false });
      if (snapshot === null) {
        sessionPhase.value = "failed";
        loading.value = false;
        return;
      }

      loading.value = false;
      await nextTick();

      sessionSnapshot.value = await openSession();
      sessionPhase.value = "ready";

      const rescanSnapshot = await handleRescanRuntimeDisks({ showLoading: false });
      if (rescanSnapshot === null) {
        sessionPhase.value = "failed";
        return;
      }

      diskDisplayPhase.value = "normal";
      await nextTick();

      await runInitialAutoConnect(rescanSnapshot);
    } catch (error) {
      sessionSnapshot.value = null;
      sessionPhase.value = "failed";
      errorText.value = getErrorMessage(error);
    } finally {
      loading.value = false;
    }
  }

  onMounted(() => {
    void bootstrapHomePage();
  });

  return {
    actionLoadingDiskId,
    autoConnectCount,
    diskDisplayPhase,
    errorText,
    handleConnectDisk,
    handleRescanRuntimeDisks,
    loadHomeDiskList,
    loading,
    runtimeDisks,
    sessionPhase,
    sessionSnapshot,
  };
}
