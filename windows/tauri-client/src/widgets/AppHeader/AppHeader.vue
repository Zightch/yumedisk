<script setup lang="ts">
import { Plus, Setting } from "@element-plus/icons-vue";

defineProps<{
  sessionReady: boolean;
}>();
</script>

<template>
  <el-row justify="space-between" align="middle" style="width: 100%">
    <el-col :span="12">
      <el-space wrap>
        <el-text>
          <strong>YumeDisk</strong>
        </el-text>
        <el-button :icon="Setting" circle disabled />
      </el-space>
    </el-col>

    <el-col :span="12" style="display: flex; justify-content: flex-end">
      <el-space wrap>
        <el-tag :type="sessionReady ? 'success' : 'info'" round>
          {{ sessionReady ? "会话正常" : "会话未就绪" }}
        </el-tag>
        <el-button :icon="Plus" circle type="primary" disabled />
      </el-space>
    </el-col>
  </el-row>
</template>
