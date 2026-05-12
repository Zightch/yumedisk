<script setup lang="ts">
defineProps<{
  diskCount: number;
  autoConnectCount: number;
}>();
</script>

<template>
  <el-card shadow="never" style="height: 100%">
    <template #header>
      <el-row justify="space-between" align="middle" :gutter="16">
        <el-col :xs="24" :sm="12">
          <el-space wrap>
            <el-text>
              <strong>磁盘列表</strong>
            </el-text>
            <el-tag size="small" round>{{ diskCount }}</el-tag>
          </el-space>
        </el-col>

        <el-col :xs="24" :sm="12" style="display: flex; justify-content: flex-end">
          <el-space wrap>
            <el-text type="info">自动连接</el-text>
            <el-text>{{ autoConnectCount }} / {{ diskCount }}</el-text>
          </el-space>
        </el-col>
      </el-row>
    </template>

    <el-scrollbar style="height: 100%">
      <el-empty description="当前还没有磁盘配置" />
    </el-scrollbar>
  </el-card>
</template>
