<script setup lang="ts">
import { Delete } from "@element-plus/icons-vue";
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

function formatCapacityText(value: number, readOnly: boolean): string {
  return `${formatBytes(value)} · ${readOnly ? "只读" : "读写"}`;
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
            {{ formatCapacityText(item.capacityBytes, item.readOnly) }}
          </div>

          <el-button
            class="network-draft-card__remove"
            aria-label="删除草稿项"
            :loading="removingRemoteDiskId === item.remoteDiskId"
            @click="emit('remove', item.remoteDiskId)"
          >
            <el-icon>
              <Delete />
            </el-icon>
          </el-button>
        </article>
      </div>
    </el-scrollbar>
  </section>
</template>

<style scoped>
.network-dialog__section--list {
  min-height: 0;
}

.network-dialog__section-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  margin-bottom: 8px;
}

.network-dialog__section-title {
  margin: 0;
  font-size: 14px;
  font-weight: 700;
}

.network-dialog__section-count {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  min-width: 28px;
  padding: 4px 8px;
  border-radius: 999px;
  background: rgba(255, 255, 255, 0.08);
  font-size: 12px;
}

.network-dialog__scroll {
  max-height: 260px;
}

.network-dialog__empty {
  padding: 8px 0 16px;
}

.network-dialog__list {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.network-draft-card {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(120px, 148px) auto;
  gap: 16px;
  align-items: start;
  padding: 14px 18px;
  border-radius: 18px;
  background: rgba(255, 255, 255, 0.04);
  border: 1px solid rgba(255, 255, 255, 0.08);
}

.network-draft-card__identity {
  min-width: 0;
}

.network-draft-card__title {
  margin: 0;
  font-size: 15px;
  font-weight: 700;
}

.network-draft-card__disk-id {
  margin: 10px 0 0;
  color: rgba(255, 255, 255, 0.88);
  font-size: 14px;
  line-height: 1.3;
  word-break: break-all;
}

.network-draft-card__capacity {
  align-self: center;
  justify-self: center;
  font-size: 20px;
  font-weight: 600;
  line-height: 1.1;
}

.network-draft-card__remove.el-button {
  align-self: center;
  justify-self: end;
  width: 32px;
  height: 32px;
  padding: 0;
  border-radius: 8px;
  border-color: rgba(255, 255, 255, 0.08);
  background: rgba(255, 255, 255, 0.04);
  color: rgba(255, 255, 255, 0.72);
  box-shadow: none;
}

.network-draft-card__remove.el-button:hover:not(.is-disabled),
.network-draft-card__remove.el-button:focus-visible:not(.is-disabled) {
  border-color: rgba(248, 113, 113, 0.42);
  background: rgba(248, 113, 113, 0.12);
  color: #fca5a5;
}

.network-draft-card__remove.el-button.is-loading,
.network-draft-card__remove.el-button.is-loading:hover,
.network-draft-card__remove.el-button.is-loading:focus-visible {
  border-color: rgba(255, 255, 255, 0.08);
  background: rgba(255, 255, 255, 0.04);
  color: rgba(255, 255, 255, 0.72);
}

.network-draft-card__remove .el-icon {
  font-size: 14px;
}
</style>
