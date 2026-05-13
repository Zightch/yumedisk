<script setup lang="ts">
import { RefreshRight } from "@element-plus/icons-vue";
import { computed } from "vue";
import type { HomeDiskListItem } from "../../entities/disk/model";
import DiskCard from "../DiskCard/DiskCard.vue";

const props = defineProps<{
  disks: HomeDiskListItem[];
  autoConnectCount: number;
  loading: boolean;
  errorText: string | null;
  actionLoadingDiskId: string | null;
}>();

const emit = defineEmits<{
  rescan: [];
  connect: [diskId: string];
  disconnect: [diskId: string];
  delete: [diskId: string];
  edit: [diskId: string];
}>();

const diskCount = computed(() => props.disks.length);
</script>

<template>
  <section class="list-panel">
    <div class="list-panel__surface">
      <div class="list-panel__header">
        <div class="list-panel__title-group">
          <h2 class="list-panel__title">磁盘列表</h2>
          <span class="list-panel__count">{{ diskCount }}</span>
        </div>

        <div class="list-panel__meta">
          <el-button class="list-panel__rescan" text aria-label="重扫" @click="emit('rescan')">
            <el-icon>
              <RefreshRight />
            </el-icon>
          </el-button>

          <div class="list-panel__summary">
            <span class="list-panel__summary-label">自动连接</span>
            <span class="list-panel__summary-value">{{ autoConnectCount }} / {{ diskCount }}</span>
          </div>
        </div>
      </div>

      <div class="list-panel__viewport">
        <el-scrollbar class="list-panel__scroll">
          <div v-if="loading" class="list-panel__content list-panel__loading">
            <el-skeleton :rows="3" animated />
            <el-skeleton :rows="3" animated />
          </div>

          <div v-else-if="disks.length > 0" class="list-panel__content">
            <DiskCard
              v-for="disk in disks"
              :key="disk.diskId"
              :disk="disk"
              :action-loading="actionLoadingDiskId === disk.diskId"
              @connect="emit('connect', $event)"
              @disconnect="emit('disconnect', $event)"
              @edit="emit('edit', $event)"
              @delete="emit('delete', $event)"
            />
          </div>

          <el-alert
            v-else-if="errorText"
            class="list-panel__error"
            :title="errorText"
            type="error"
            :closable="false"
            show-icon
          />

          <el-empty
            v-else
            class="list-panel__empty"
            description="当前还没有磁盘配置"
            :image-size="0"
          />
        </el-scrollbar>
      </div>
    </div>
  </section>
</template>
