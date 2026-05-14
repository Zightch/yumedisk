import { nextTick, onMounted, ref } from "vue";
import type { HomeDiskListItem, HomeDiskListSnapshot } from "../../entities/disk/model";
import type { SessionSnapshot } from "../../entities/session/model";
import {
  connectDisk,
  queryHomeDiskList,
} from "../../shared/api/diskClient";
import {
  getErrorMessage,
  openSession,
  restoreClientState,
} from "../../shared/api/sessionClient";

export interface HomeBootstrapState {
  disks: HomeDiskListItem[];
  autoConnectCount: number;
  loading: boolean;
  errorText: string | null;
  sessionSnapshot: SessionSnapshot | null;
}

export function useHomeBootstrap() {
  const disks = ref<HomeDiskListItem[]>([]);
  const autoConnectCount = ref(0);
  const loading = ref(true);
  const errorText = ref<string | null>(null);
  const sessionSnapshot = ref<SessionSnapshot | null>(null);
  const initialAutoConnectCompleted = ref(false);
  const actionLoadingDiskId = ref<string | null>(null);

  async function loadHomeDiskList(options: { showLoading?: boolean } = {}): Promise<HomeDiskListSnapshot | null> {
    const showLoading = options.showLoading ?? true;
    if (showLoading) {
      loading.value = true;
    }

    errorText.value = null;

    try {
      const snapshot = await queryHomeDiskList();
      disks.value = snapshot.disks;
      autoConnectCount.value = snapshot.autoConnectCount;
      return snapshot;
    } catch (error) {
      disks.value = [];
      autoConnectCount.value = 0;
      errorText.value = getErrorMessage(error);
      return null;
    } finally {
      if (showLoading) {
        loading.value = false;
      }
    }
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

    try {
      await restoreClientState();

      const snapshot = await loadHomeDiskList({ showLoading: false });
      if (snapshot === null) {
        sessionSnapshot.value = null;
        return;
      }

      sessionSnapshot.value = await openSession();

      await nextTick();
      await runInitialAutoConnect(snapshot);
    } catch (error) {
      sessionSnapshot.value = null;
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
    disks,
    errorText,
    handleConnectDisk,
    loadHomeDiskList,
    loading,
    sessionSnapshot,
  };
}
