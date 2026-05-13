<script setup lang="ts">
import { ElMessage } from "element-plus";
import { nextTick, onMounted, ref } from "vue";
import type { HomeDiskListItem, HomeDiskListSnapshot } from "../../entities/disk/model";
import CreateFileDiskDialog from "../../features/createFileDisk/CreateFileDiskDialog.vue";
import CreateMemoryDiskDialog from "../../features/createMemoryDisk/CreateMemoryDiskDialog.vue";
import EditDiskDialog from "../../features/editDisk/EditDiskDialog.vue";
import { DEFAULT_THEME, applyTheme, type AppTheme } from "../../shared/theme/theme";
import {
  connectDisk,
  deleteDisk,
  disconnectDisk,
  queryHomeDiskList,
  rescanRuntimeDisks,
} from "../../shared/api/diskClient";
import { getErrorMessage } from "../../shared/api/sessionClient";
import AppHeader from "../../widgets/AppHeader/AppHeader.vue";
import DiskListPanel from "../../widgets/DiskListPanel/DiskListPanel.vue";
import SettingsPage from "../../widgets/SettingsPage/SettingsPage.vue";

defineProps<{
  sessionReady: boolean;
}>();

const disks = ref<HomeDiskListItem[]>([]);
const autoConnectCount = ref(0);
const loading = ref(true);
const errorText = ref<string | null>(null);
const memoryCreateVisible = ref(false);
const fileCreateVisible = ref(false);
const editDiskVisible = ref(false);
const settingsVisible = ref(false);
const editingDisk = ref<HomeDiskListItem | null>(null);
const actionLoadingDiskId = ref<string | null>(null);
const currentTheme = ref<AppTheme>({ ...DEFAULT_THEME });
const initialAutoConnectCompleted = ref(false);

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
  const snapshot = await loadHomeDiskList({ showLoading: true });
  if (snapshot === null) {
    return;
  }

  await nextTick();
  await runInitialAutoConnect(snapshot);
}

onMounted(() => {
  void bootstrapHomePage();
});

function handleOpenMemoryCreate() {
  memoryCreateVisible.value = true;
}

function handleOpenFileCreate() {
  fileCreateVisible.value = true;
}

function handleOpenSettings() {
  settingsVisible.value = true;
}

async function handleMemoryDiskCreated() {
  await loadHomeDiskList();
}

async function handleFileDiskCreated() {
  await loadHomeDiskList();
}

function handleEditDisk(diskId: string) {
  const disk = disks.value.find((item) => item.diskId === diskId) ?? null;
  if (disk === null) {
    ElMessage.error("磁盘不存在");
    return;
  }

  editingDisk.value = disk;
  editDiskVisible.value = true;
}

async function handleDiskUpdated() {
  editingDisk.value = null;
  await loadHomeDiskList();
}

async function handleConnectDisk(
  diskId: string,
  options: { silentSuccess?: boolean } = {},
) {
  actionLoadingDiskId.value = diskId;

  try {
    await connectDisk({ diskId });
    if (!options.silentSuccess) {
      ElMessage.success("磁盘已连接");
    }
    await loadHomeDiskList({ showLoading: false });
  } catch (error) {
    const message = getErrorMessage(error);
    ElMessage.error(options.silentSuccess ? `自动连接失败：${message}` : message);
  } finally {
    actionLoadingDiskId.value = null;
  }
}

async function handleDisconnectDisk(diskId: string) {
  actionLoadingDiskId.value = diskId;

  try {
    await disconnectDisk({ diskId });
    ElMessage.success("磁盘已断开");
    await loadHomeDiskList({ showLoading: false });
  } catch (error) {
    ElMessage.error(getErrorMessage(error));
  } finally {
    actionLoadingDiskId.value = null;
  }
}

async function handleDeleteDisk(diskId: string) {
  actionLoadingDiskId.value = diskId;

  try {
    await deleteDisk({ diskId });
    ElMessage.success("磁盘已删除");
    await loadHomeDiskList({ showLoading: false });
  } catch (error) {
    ElMessage.error(getErrorMessage(error));
  } finally {
    actionLoadingDiskId.value = null;
  }
}

async function handleRescanRuntimeDisks() {
  loading.value = true;
  errorText.value = null;

  try {
    const snapshot = await rescanRuntimeDisks();
    disks.value = snapshot.disks;
    autoConnectCount.value = snapshot.autoConnectCount;
    ElMessage.success("已完成重扫");
  } catch (error) {
    errorText.value = getErrorMessage(error);
    ElMessage.error(errorText.value);
  } finally {
    loading.value = false;
  }
}

function handleThemeChanged(theme: AppTheme) {
  currentTheme.value = theme;
  applyTheme(theme);
}
</script>

<template>
  <el-container class="app-shell home-page" direction="vertical">
    <el-header class="home-page__header" height="auto">
      <AppHeader
        :session-ready="sessionReady"
        @open-settings="handleOpenSettings"
        @open-memory-create="handleOpenMemoryCreate"
        @open-file-create="handleOpenFileCreate"
      />
    </el-header>

    <el-main class="home-page__main">
      <DiskListPanel
        :disks="disks"
        :auto-connect-count="autoConnectCount"
        :loading="loading"
        :error-text="errorText"
        :action-loading-disk-id="actionLoadingDiskId"
        @rescan="handleRescanRuntimeDisks"
        @connect="handleConnectDisk"
        @disconnect="handleDisconnectDisk"
        @edit="handleEditDisk"
        @delete="handleDeleteDisk"
      />
    </el-main>
  </el-container>

  <SettingsPage
    v-model="settingsVisible"
    :theme="currentTheme"
    @update:theme="handleThemeChanged"
  />
  <CreateMemoryDiskDialog
    v-model="memoryCreateVisible"
    @created="handleMemoryDiskCreated"
  />
  <CreateFileDiskDialog
    v-model="fileCreateVisible"
    @created="handleFileDiskCreated"
  />
  <EditDiskDialog
    v-model="editDiskVisible"
    :disk="editingDisk"
    @updated="handleDiskUpdated"
  />
</template>
