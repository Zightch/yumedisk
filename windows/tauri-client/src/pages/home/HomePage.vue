<script setup lang="ts">
import { ElMessage } from "element-plus";
import { computed, ref } from "vue";
import type { HomeDiskListItem } from "../../entities/disk/model";
import CreateFileDiskDialog from "../../features/createFileDisk/CreateFileDiskDialog.vue";
import CreateMemoryDiskDialog from "../../features/createMemoryDisk/CreateMemoryDiskDialog.vue";
import EditDiskDialog from "../../features/editDisk/EditDiskDialog.vue";
import { mapHomeDiskDisplayItems } from "../../features/homeBootstrap/homeDisplayMapper";
import { useHomeBootstrap } from "../../features/homeBootstrap/useHomeBootstrap";
import { DEFAULT_THEME, applyTheme, type AppTheme } from "../../shared/theme/theme";
import {
  deleteDisk,
  disconnectDisk,
} from "../../shared/api/diskClient";
import { getErrorMessage } from "../../shared/api/sessionClient";
import AppHeader from "../../widgets/AppHeader/AppHeader.vue";
import DiskListPanel from "../../widgets/DiskListPanel/DiskListPanel.vue";
import SettingsPage from "../../widgets/SettingsPage/SettingsPage.vue";
const memoryCreateVisible = ref(false);
const fileCreateVisible = ref(false);
const editDiskVisible = ref(false);
const settingsVisible = ref(false);
const editingDisk = ref<HomeDiskListItem | null>(null);
const currentTheme = ref<AppTheme>({ ...DEFAULT_THEME });

const {
  actionLoadingDiskId,
  autoConnectCount,
  diskDisplayPhase,
  errorText,
  handleConnectDisk: runConnectDisk,
  handleRescanRuntimeDisks: runRescanRuntimeDisks,
  loadHomeDiskList,
  loading,
  runtimeDisks,
  sessionPhase,
} = useHomeBootstrap();

const displayDisks = computed(() => mapHomeDiskDisplayItems(runtimeDisks.value, {
  sessionPhase: sessionPhase.value,
  diskDisplayPhase: diskDisplayPhase.value,
}));

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
  const disk = runtimeDisks.value.find((item) => item.diskId === diskId) ?? null;
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
  const { ok, errorText: message } = await runConnectDisk(diskId, options);

  if (ok) {
    if (!options.silentSuccess) {
      ElMessage.success("磁盘已连接");
    }
    return;
  }

  if (message) {
    ElMessage.error(message);
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
  try {
    const snapshot = await runRescanRuntimeDisks();
    if (snapshot === null) {
      return;
    }

    ElMessage.success("已完成重扫");
  } catch (error) {
    errorText.value = getErrorMessage(error);
    ElMessage.error(errorText.value);
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
        :session-phase="sessionPhase"
        @open-settings="handleOpenSettings"
        @open-memory-create="handleOpenMemoryCreate"
        @open-file-create="handleOpenFileCreate"
      />
    </el-header>

    <el-main class="home-page__main">
      <DiskListPanel
        :disks="displayDisks"
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
