<script setup lang="ts">
import AppHeader from "../../widgets/AppHeader/AppHeader.vue";
import DiskListPanel from "../../widgets/DiskListPanel/DiskListPanel.vue";

defineProps<{
  sessionReady: boolean;
}>();
</script>

<template>
  <el-container direction="vertical" style="min-height: 100vh">
    <el-header height="auto">
      <AppHeader :session-ready="sessionReady" />
    </el-header>
    <el-main>
      <DiskListPanel :disk-count="0" :auto-connect-count="0" />
    </el-main>
  </el-container>
</template>
