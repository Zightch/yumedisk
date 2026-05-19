<script setup lang="ts">
import type { NetworkDraftItem } from "../../entities/disk/model";

defineProps<{
  items: NetworkDraftItem[];
  errorText: string | null;
  removingRemoteDiskId: string | null;
}>();

const emit = defineEmits<{
  remove: [remoteDiskId: string];
}>();

function formatBytes(value: number): string {
  if (value <= 0) {
    return "0 B";
  }

  const units = ["B", "KB", "MB", "GB", "TB"] as const;
  let size = value;
  let unitIndex = 0;

  while (size >= 1024 && unitIndex < units.length - 1) {
    size /= 1024;
    unitIndex += 1;
  }

  const text = size >= 100 || unitIndex === 0 ? size.toFixed(0) : size.toFixed(1);
  return `${text} ${units[unitIndex]}`;
}
</script>

<template>
  <el-alert
    v-if="errorText"
    class="app-dialog__alert"
    :title="errorText"
    type="error"
    :closable="false"
    show-icon
  />

  <section class="network-dialog__section network-dialog__section--list">
    <div class="network-dialog__section-header">
      <h4 class="network-dialog__section-title">待提交磁盘</h4>
      <span class="network-dialog__section-count">{{ items.length }}</span>
    </div>

    <el-scrollbar class="network-dialog__scroll">
      <div v-if="items.length === 0" class="network-dialog__empty">
        <el-empty description="先测试连接，再添加网络盘" :image-size="0" />
      </div>

      <div v-else class="network-dialog__list">
        <article
          v-for="item in items"
          :key="item.remoteDiskId"
          class="network-draft-card"
        >
          <div class="network-draft-card__identity">
            <h5 class="network-draft-card__title">{{ item.diskName }}</h5>
            <p class="network-draft-card__disk-id">{{ item.remoteDiskId }}</p>
          </div>

          <div class="network-draft-card__capacity">
            {{ formatBytes(item.capacityBytes) }}
          </div>

          <el-button
            class="network-draft-card__remove"
            :loading="removingRemoteDiskId === item.remoteDiskId"
            @click="emit('remove', item.remoteDiskId)"
          >
            删除
          </el-button>
        </article>
      </div>
    </el-scrollbar>
  </section>
</template>
