<script setup lang="ts">
import { computed } from "vue";
import type { HomeDiskListItem } from "../../entities/disk/model";
import {
  formatDiskCapacityText,
  formatDiskDetailText,
  formatDiskKindText,
} from "../../entities/disk/presenter";

const props = defineProps<{
  disks: HomeDiskListItem[];
  autoConnectCount: number;
  loading: boolean;
  errorText: string | null;
  actionLoadingDiskId: string | null;
}>();

const emit = defineEmits<{
  connect: [diskId: string];
  disconnect: [diskId: string];
  delete: [diskId: string];
}>();

const diskCount = computed(() => props.disks.length);
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
      <el-space v-if="loading" direction="vertical" fill>
        <el-skeleton :rows="3" animated />
        <el-skeleton :rows="3" animated />
      </el-space>

      <el-space v-else-if="disks.length > 0" direction="vertical" fill>
        <el-card
          v-for="disk in disks"
          :key="disk.diskId"
          shadow="never"
        >
          <el-space direction="vertical" fill>
            <el-row justify="space-between" align="middle" :gutter="16">
              <el-col :xs="24" :sm="16">
                <el-space direction="vertical" fill size="small">
                  <el-text>
                    <strong>{{ disk.diskName }}</strong>
                  </el-text>
                  <el-space wrap size="small">
                    <el-tag size="small" effect="plain">
                      {{ formatDiskKindText(disk) }}
                    </el-tag>
                    <el-text type="info">{{ formatDiskCapacityText(disk) }}</el-text>
                    <el-tag v-if="disk.readOnly" size="small" type="warning" effect="plain">
                      只读
                    </el-tag>
                    <el-tag v-if="!disk.valid" size="small" type="danger" effect="plain">
                      无效
                    </el-tag>
                    <el-tag v-if="disk.autoConnect" size="small" type="info" effect="plain">
                      自动连接
                    </el-tag>
                  </el-space>
                </el-space>
              </el-col>

              <el-col :xs="24" :sm="8" style="display: flex; justify-content: flex-end">
                <el-space wrap>
                  <el-tag :type="disk.connected ? 'success' : 'info'" round>
                    {{ disk.connected ? "已连接" : "未连接" }}
                  </el-tag>
                  <el-button
                    v-if="!disk.connected"
                    type="primary"
                    plain
                    size="small"
                    :disabled="!disk.valid"
                    :loading="actionLoadingDiskId === disk.diskId"
                    @click="emit('connect', disk.diskId)"
                  >
                    连接
                  </el-button>
                  <el-button
                    v-else
                    type="danger"
                    plain
                    size="small"
                    :loading="actionLoadingDiskId === disk.diskId"
                    @click="emit('disconnect', disk.diskId)"
                  >
                    断开
                  </el-button>
                  <el-button
                    type="danger"
                    plain
                    size="small"
                    :loading="actionLoadingDiskId === disk.diskId"
                    @click="emit('delete', disk.diskId)"
                  >
                    删除
                  </el-button>
                </el-space>
              </el-col>
            </el-row>

            <el-text v-if="formatDiskDetailText(disk)" type="info" truncated>
              {{ formatDiskDetailText(disk) }}
            </el-text>
          </el-space>
        </el-card>
      </el-space>

      <el-alert
        v-else-if="errorText"
        :title="errorText"
        type="error"
        :closable="false"
        show-icon
      />

      <el-empty v-else description="当前还没有磁盘配置" />
    </el-scrollbar>
  </el-card>
</template>
