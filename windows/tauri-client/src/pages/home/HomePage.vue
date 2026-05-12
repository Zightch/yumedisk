<script setup lang="ts">
import { onMounted, ref } from "vue";
import type { HomeDiskListItem } from "../../entities/disk/model";
import CreateMemoryDiskDialog from "../../features/createMemoryDisk/CreateMemoryDiskDialog.vue";
import { queryHomeDiskList } from "../../shared/api/diskClient";
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

async function handleMemoryDiskCreated() {
  await loadHomeDiskList();
}
</script>

<template>
  <el-container direction="vertical" style="min-height: 100vh">
    <el-header height="auto" style="padding: 16px 16px 0 16px">
      <AppHeader
        :session-ready="sessionReady"
        @open-memory-create="handleOpenMemoryCreate"
      />
    </el-header>

    <el-main style="padding: 16px; overflow: hidden">
      <DiskListPanel
        :disks="disks"
        :auto-connect-count="autoConnectCount"
        :loading="loading"
        :error-text="errorText"
      />
    </el-main>
  </el-container>

  <CreateMemoryDiskDialog
    v-model="memoryCreateVisible"
    @created="handleMemoryDiskCreated"
  />
</template>
