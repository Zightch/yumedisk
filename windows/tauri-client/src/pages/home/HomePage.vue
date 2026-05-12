<script setup lang="ts">
import { ElMessage } from "element-plus";
import { onMounted, ref } from "vue";
import type { HomeDiskListItem } from "../../entities/disk/model";
import CreateFileDiskDialog from "../../features/createFileDisk/CreateFileDiskDialog.vue";
import CreateMemoryDiskDialog from "../../features/createMemoryDisk/CreateMemoryDiskDialog.vue";
import {
  connectDisk,
  deleteDisk,
  disconnectDisk,
  queryHomeDiskList,
} from "../../shared/api/diskClient";
import { getErrorMessage } from "../../shared/api/sessionClient";
import AppHeader from "../../widgets/AppHeader/AppHeader.vue";
import DiskListPanel from "../../widgets/DiskListPanel/DiskListPanel.vue";

defineProps<{
  sessionReady: boolean;
}>();

const disks = ref<HomeDiskListItem[]>([]);
const autoConnectCount = ref(0);
const loading = ref(true);
const errorText = ref<string | null>(null);
const memoryCreateVisible = ref(false);
const fileCreateVisible = ref(false);
const actionLoadingDiskId = ref<string | null>(null);

async function loadHomeDiskList() {
  loading.value = true;
  errorText.value = null;

  try {
    const snapshot = await queryHomeDiskList();
    disks.value = snapshot.disks;
    autoConnectCount.value = snapshot.autoConnectCount;
  } catch (error) {
    disks.value = [];
    autoConnectCount.value = 0;
    errorText.value = getErrorMessage(error);
  } finally {
    loading.value = false;
  }
}

onMounted(() => {
  void loadHomeDiskList();
});

function handleOpenMemoryCreate() {
  memoryCreateVisible.value = true;
}

function handleOpenFileCreate() {
  fileCreateVisible.value = true;
}

async function handleMemoryDiskCreated() {
  await loadHomeDiskList();
}

async function handleFileDiskCreated() {
  await loadHomeDiskList();
}

async function handleConnectDisk(diskId: string) {
  actionLoadingDiskId.value = diskId;

  try {
    await connectDisk({ diskId });
    ElMessage.success("磁盘已连接");
    await loadHomeDiskList();
  } catch (error) {
    ElMessage.error(getErrorMessage(error));
  } finally {
    actionLoadingDiskId.value = null;
  }
}

async function handleDisconnectDisk(diskId: string) {
  actionLoadingDiskId.value = diskId;

  try {
    await disconnectDisk({ diskId });
    ElMessage.success("磁盘已断开");
    await loadHomeDiskList();
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
    await loadHomeDiskList();
  } catch (error) {
    ElMessage.error(getErrorMessage(error));
  } finally {
    actionLoadingDiskId.value = null;
  }
}
</script>

<template>
  <el-container direction="vertical" style="min-height: 100vh">
    <el-header height="auto" style="padding: 16px 16px 0 16px">
      <AppHeader
        :session-ready="sessionReady"
        @open-memory-create="handleOpenMemoryCreate"
        @open-file-create="handleOpenFileCreate"
      />
    </el-header>

    <el-main style="padding: 16px; overflow: hidden">
      <DiskListPanel
        :disks="disks"
        :auto-connect-count="autoConnectCount"
        :loading="loading"
        :error-text="errorText"
        :action-loading-disk-id="actionLoadingDiskId"
        @connect="handleConnectDisk"
        @disconnect="handleDisconnectDisk"
        @delete="handleDeleteDisk"
      />
    </el-main>
  </el-container>

  <CreateMemoryDiskDialog
    v-model="memoryCreateVisible"
    @created="handleMemoryDiskCreated"
  />
  <CreateFileDiskDialog
    v-model="fileCreateVisible"
    @created="handleFileDiskCreated"
  />
</template>
