<script setup lang="ts">
import { computed } from "vue";

const props = defineProps<{
  serverAddr: string;
  diskName: string;
  claimCode: string;
  canEditDraft: boolean;
  canAddItem: boolean;
  testing: boolean;
  adding: boolean;
  submitting: boolean;
  removingRemoteDiskId: string | null;
  connectionStatusText: string;
  connectionStatusClass: string;
}>();

const emit = defineEmits<{
  "update:serverAddr": [value: string];
  "update:diskName": [value: string];
  "update:claimCode": [value: string];
  testConnection: [];
  addItem: [];
}>();

const serverAddrModel = computed({
  get: () => props.serverAddr,
  set: (value: string) => emit("update:serverAddr", value),
});

const diskNameModel = computed({
  get: () => props.diskName,
  set: (value: string) => emit("update:diskName", value),
});

const claimCodeModel = computed({
  get: () => props.claimCode,
  set: (value: string) => emit("update:claimCode", value),
});
</script>

<template>
  <section class="network-dialog__section">
    <el-form class="app-dialog-form" label-position="top">
      <el-form-item>
        <template #label>
          <div class="network-dialog__server-label">
            <span class="network-dialog__server-label-text">服务器地址</span>
            <span
              class="network-dialog__status-badge network-dialog__status-badge--compact"
              :class="connectionStatusClass"
            >
              {{ connectionStatusText }}
            </span>
          </div>
        </template>
        <div class="network-dialog__server-row">
          <el-input
            v-model="serverAddrModel"
            placeholder="输入服务器 IP 或地址"
            :disabled="testing || submitting || adding || removingRemoteDiskId !== null"
          />
          <el-button
            class="network-dialog__test-button network-dialog__utility-button"
            type="primary"
            :loading="testing"
            :disabled="submitting || adding || removingRemoteDiskId !== null"
            @click="emit('testConnection')"
          >
            测试连接
          </el-button>
        </div>
      </el-form-item>
    </el-form>
  </section>

  <el-divider class="network-dialog__divider" />

  <section class="network-dialog__section">
    <el-form class="app-dialog-form" label-position="top">
      <el-form-item label="磁盘名称">
        <el-input
          v-model="diskNameModel"
          placeholder="输入磁盘名称"
          :disabled="!canEditDraft"
        />
      </el-form-item>

      <el-form-item label="领盘码">
        <el-input
          v-model="claimCodeModel"
          placeholder="输入领盘码"
          :disabled="!canEditDraft"
        />
      </el-form-item>
    </el-form>

    <div class="network-dialog__actions">
      <el-button
        class="network-dialog__action-button network-dialog__utility-button"
        type="primary"
        :loading="adding"
        :disabled="!canAddItem || testing || submitting || removingRemoteDiskId !== null"
        @click="emit('addItem')"
      >
        添加
      </el-button>
    </div>
  </section>
</template>

<style scoped>
.network-dialog__section {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.network-dialog__server-label {
  display: flex;
  align-items: center;
  gap: 8px;
  min-width: 0;
}

.network-dialog__server-label-text {
  flex: 0 0 auto;
}

.network-dialog__server-row {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  gap: 12px;
  align-items: center;
  width: 100%;
}

.network-dialog__server-row .el-input {
  min-width: 0;
}

.network-dialog__status-badge {
  display: inline-flex;
  align-items: center;
  justify-content: flex-start;
  width: fit-content;
  min-width: 0;
  max-width: 100%;
  padding: 6px 12px;
  border-radius: 999px;
  font-size: 12px;
  font-weight: 600;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  text-align: left;
}

.network-dialog__status-badge--idle {
  background: rgba(255, 255, 255, 0.08);
  color: rgba(255, 255, 255, 0.84);
}

.network-dialog__status-badge--testing {
  background: rgba(245, 158, 11, 0.18);
  color: #fbbf24;
}

.network-dialog__status-badge--ready {
  background: rgba(34, 197, 94, 0.18);
  color: #4ade80;
}

.network-dialog__status-badge--compact {
  padding: 3px 8px;
  font-size: 11px;
  line-height: 1.2;
}

.network-dialog__utility-button.el-button {
  padding: 0 14px;
  border-radius: 8px;
  font-size: 12px;
  font-weight: 600;
  box-shadow: none;
  --el-button-bg-color: var(--accent-soft);
  --el-button-border-color: var(--accent-border);
  --el-button-text-color: var(--text-primary);
  --el-button-hover-bg-color: color-mix(in srgb, var(--accent-soft) 82%, var(--accent) 18%);
  --el-button-hover-border-color: color-mix(in srgb, var(--accent-border) 82%, white 18%);
  --el-button-hover-text-color: var(--text-primary);
  --el-button-active-bg-color: color-mix(in srgb, var(--accent-soft) 72%, var(--accent) 28%);
  --el-button-active-border-color: color-mix(in srgb, var(--accent-border) 76%, white 12%);
  --el-button-active-text-color: var(--text-primary);
  --el-button-disabled-bg-color: rgba(255, 255, 255, 0.05);
  --el-button-disabled-border-color: var(--border-soft);
  --el-button-disabled-text-color: var(--text-muted);
}

.network-dialog__test-button {
  justify-self: end;
}

.network-dialog__divider {
  margin: 8px 0 0;
}

.network-dialog__actions {
  display: flex;
  justify-content: flex-end;
  margin-top: 2px;
}

.network-dialog__action-button {
  min-width: 84px;
}
</style>
