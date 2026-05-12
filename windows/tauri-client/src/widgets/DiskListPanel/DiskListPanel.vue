<script setup lang="ts">
defineProps<{
  diskCount: number;
  autoConnectCount: number;
}>();
</script>

<template>
  <el-card shadow="never">
    <template #header>
      <el-row justify="space-between" align="middle">
        <el-col :span="12">
          <el-space wrap>
            <el-text>
              <strong>磁盘列表</strong>
            </el-text>
            <el-tag size="small" round>{{ diskCount }}</el-tag>
          </el-space>
        </el-col>

        <el-col :span="12" style="display: flex; justify-content: flex-end">
          <el-space wrap>
            <el-text type="info">自动连接</el-text>
            <el-text>{{ autoConnectCount }} / {{ diskCount }}</el-text>
          </el-space>
        </el-col>
      </el-row>
    </template>

    <el-empty description="当前还没有磁盘配置" />
  </el-card>
</template>
