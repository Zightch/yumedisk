<script setup lang="ts">
import { ElMessage } from "element-plus";
import { computed, ref } from "vue";
import type { HomeDiskListItem } from "../../entities/disk/model";
import CreateFileDiskDialog from "../../features/createFileDisk/CreateFileDiskDialog.vue";
import CreateMemoryDiskDialog from "../../features/createMemoryDisk/CreateMemoryDiskDialog.vue";
import EditDiskDialog from "../../features/editDisk/EditDiskDialog.vue";
import { mapHomeDiskDisplayItems } from "../../features/homeBootstrap/homeDisplayMapper";
import { useHomeBootstrap } from "../../features/homeBootstrap/useHomeBootstrap";
import { useRemoveDiskFlow } from "../../features/removeDisk/useRemoveDiskFlow";
import AppSessionStatusDialog from "../../features/appSessionStatus/AppSessionStatusDialog.vue";
import { DEFAULT_THEME, applyTheme, type AppTheme } from "../../shared/theme/theme";
import {
  ejectDisk,
} from "../../shared/api/diskClient";
import { getErrorMessage } from "../../shared/api/appSessionClient";
import AppHeader from "../../widgets/AppHeader/AppHeader.vue";
import DiskListPanel from "../../widgets/DiskListPanel/DiskListPanel.vue";
import SettingsPage from "../../widgets/SettingsPage/SettingsPage.vue";
const memoryCreateVisible = ref(false);
const fileCreateVisible = ref(false);
const editDiskVisible = ref(false);
const settingsVisible = ref(false);
const appSessionStatusVisible = ref(false);
const editingDisk = ref<HomeDiskListItem | null>(null);
const currentTheme = ref<AppTheme>({ ...DEFAULT_THEME });

const {
  actionLoadingDiskId,
  autoMountCount,
  diskDisplayPhase,
  errorText,
  handleMountDisk: runMountDisk,
  handleRescanRuntimeDisks: runRescanRuntimeDisks,
  loadHomeDiskList,
  loading,
  retryOpenAppSessionFlow,
  runtimeDisks,
  appSessionPhase,
  appSessionStatusText,
} = useHomeBootstrap();

const displayDisks = computed(() => mapHomeDiskDisplayItems(runtimeDisks.value, {
  appSessionPhase: appSessionPhase.value,
  diskDisplayPhase: diskDisplayPhase.value,
}));
const { removeDisk } = useRemoveDiskFlow({
  actionLoadingDiskId,
  loadHomeDiskList,
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

function handleOpenAppSessionStatus() {
  appSessionStatusVisible.value = true;
}

async function handleMemoryDiskCreated() {
  await loadHomeDiskList();
}

async function handleFileDiskCreated() {
  await loadHomeDiskList();
}

function handleEditDisk(localDiskId: string) {
  const disk = runtimeDisks.value.find((item) => item.localDiskId === localDiskId) ?? null;
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

async function handleMountDisk(
  localDiskId: string,
  options: { silentSuccess?: boolean } = {},
) {
  const { ok, errorText: message } = await runMountDisk(localDiskId, options);

  if (ok) {
    if (!options.silentSuccess) {
      ElMessage.success("磁盘已挂载");
    }
    return;
  }

  if (message) {
    ElMessage.error(message);
  }
}

async function handleEjectDisk(localDiskId: string) {
  actionLoadingDiskId.value = localDiskId;

  try {
    await ejectDisk({ localDiskId });
    ElMessage.success("磁盘已拔出");
    await loadHomeDiskList({ showLoading: false });
  } catch (error) {
    ElMessage.error(getErrorMessage(error));
  } finally {
    actionLoadingDiskId.value = null;
  }
}

async function handleDeleteDisk(localDiskId: string) {
  const disk = runtimeDisks.value.find((item) => item.localDiskId === localDiskId) ?? null;
  if (disk === null) {
    ElMessage.error("磁盘不存在");
    return;
  }

  await removeDisk(disk);
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

async function handleRetryAppSession() {
  await retryOpenAppSessionFlow();
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
        :app-session-phase="appSessionPhase"
        @open-app-session-status="handleOpenAppSessionStatus"
        @open-settings="handleOpenSettings"
        @open-memory-create="handleOpenMemoryCreate"
        @open-file-create="handleOpenFileCreate"
      />
    </el-header>

    <el-main class="home-page__main">
      <DiskListPanel
        :disks="displayDisks"
        :auto-mount-count="autoMountCount"
        :loading="loading"
        :error-text="errorText"
        :action-loading-disk-id="actionLoadingDiskId"
        @rescan="handleRescanRuntimeDisks"
        @mount="handleMountDisk"
        @eject="handleEjectDisk"
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
  <AppSessionStatusDialog
    v-model="appSessionStatusVisible"
    :app-session-phase="appSessionPhase"
    :app-session-status-text="appSessionStatusText"
    @retry="handleRetryAppSession"
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
