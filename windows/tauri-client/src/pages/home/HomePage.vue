<script setup lang="ts">
import AppHeader from "../../widgets/AppHeader/AppHeader.vue";
import DiskListPanel from "../../widgets/DiskListPanel/DiskListPanel.vue";

defineProps<{
  sessionReady: boolean;
}>();
</script>

<template>
  <el-container direction="vertical" style="min-height: 100vh">
    <el-header height="auto" style="padding: 16px 16px 0 16px">
      <AppHeader :session-ready="sessionReady" />
    </el-header>

    <el-main style="padding: 16px; overflow: hidden">
      <DiskListPanel :disk-count="0" :auto-connect-count="0" />
    </el-main>
  </el-container>
</template>
