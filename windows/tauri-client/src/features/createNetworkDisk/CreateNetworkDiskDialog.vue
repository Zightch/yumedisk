<script setup lang="ts">
import { computed } from "vue";
import NetworkDraftForm from "./NetworkDraftForm.vue";
import NetworkDraftList from "./NetworkDraftList.vue";
import { useNetworkDraftFlow } from "./useNetworkDraftFlow";

const props = defineProps<{
  modelValue: boolean;
}>();

const emit = defineEmits<{
  "update:modelValue": [value: boolean];
  created: [];
}>();

const dialogVisible = computed({
  get: () => props.modelValue,
  set: (value: boolean) => emit("update:modelValue", value),
});

const {
  adding,
  canAddItem,
  canEditDraft,
  canSubmit,
  connectionStatusClass,
  connectionStatusText,
  draftItems,
  errorText,
  form,
  handleAddDraftItem,
  handleCancel,
  handleRemoveDraftItem,
  handleSubmit,
  handleTestConnection,
  removingRemoteDiskId,
  submitting,
  testing,
} = useNetworkDraftFlow({
  visible: dialogVisible,
  closeDialog: () => {
    dialogVisible.value = false;
  },
  onCreated: () => emit("created"),
});
</script>

<template>
  <el-dialog
    v-model="dialogVisible"
    class="app-dialog app-dialog--network"
    :style="{ '--app-dialog-width': '760px' }"
    modal-class="app-dialog-overlay"
    transition="app-dialog-slide-horizontal"
    align-center
    :show-close="false"
  >
    <template #header>
      <div class="app-dialog__header">
        <h3 class="app-dialog__title">创建网络盘</h3>
        <el-button class="app-dialog__close" text circle aria-label="关闭" @click="handleCancel">
          ×
        </el-button>
      </div>
    </template>

    <div class="app-dialog__body network-dialog">
      <div class="app-dialog__viewport network-dialog__viewport">
        <div class="app-dialog__content network-dialog__content">
          <NetworkDraftForm
            :server-addr="form.serverAddr"
            :disk-name="form.diskName"
            :claim-code="form.claimCode"
            :can-edit-draft="canEditDraft"
            :can-add-item="canAddItem"
            :testing="testing"
            :adding="adding"
            :submitting="submitting"
            :removing-remote-disk-id="removingRemoteDiskId"
            :connection-status-text="connectionStatusText"
            :connection-status-class="connectionStatusClass"
            @update:server-addr="form.serverAddr = $event"
            @update:disk-name="form.diskName = $event"
            @update:claim-code="form.claimCode = $event"
            @test-connection="handleTestConnection"
            @add-item="handleAddDraftItem"
          />

          <NetworkDraftList
            :items="draftItems"
            :error-text="errorText"
            :removing-remote-disk-id="removingRemoteDiskId"
            @remove="handleRemoveDraftItem"
          />

          <div class="app-dialog__footer app-dialog__footer--embedded">
            <el-button
              class="app-dialog__button app-dialog__button--secondary"
              @click="handleCancel"
            >
              取消
            </el-button>
            <el-button
              class="app-dialog__button"
              type="primary"
              :loading="submitting"
              :disabled="!canSubmit"
              @click="handleSubmit"
            >
              提交
            </el-button>
          </div>
        </div>
      </div>
    </div>
  </el-dialog>
</template>

<style scoped>
.network-dialog {
  display: flex;
  flex-direction: column;
  gap: 16px;
  min-height: 0;
}

.network-dialog__viewport {
  min-height: 0;
}

.network-dialog__content {
  display: flex;
  flex-direction: column;
  gap: 16px;
  min-height: 0;
}

.network-dialog__section {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.network-dialog__server-label {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  min-width: 0;
}

.network-dialog__server-label-text {
  min-width: 0;
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
  justify-content: center;
  min-width: 0;
  max-width: 180px;
  padding: 6px 12px;
  border-radius: 999px;
  font-size: 12px;
  font-weight: 600;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
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
  max-width: 140px;
  font-size: 11px;
  line-height: 1.2;
}

.network-dialog__utility-button.el-button {
  height: 36px;
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
  min-width: 92px;
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

.network-dialog__section--list {
  min-height: 0;
}

.network-dialog__section-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
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
  grid-template-columns: minmax(0, 1fr) minmax(110px, 140px) auto;
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
}

.network-draft-card__remove {
  align-self: center;
  justify-self: end;
  min-width: 92px;
}
</style>
