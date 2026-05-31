<script setup lang="ts">
import { computed, toRef } from "vue";
import NetworkDraftForm from "./NetworkDraftForm.vue";
import NetworkDraftList from "./NetworkDraftList.vue";
import { useNetworkDraftFlow } from "./useNetworkDraftFlow";

const props = defineProps<{
  modelValue: boolean;
  interactionLocked: boolean;
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
  interactionLocked: toRef(props, "interactionLocked"),
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
            :interaction-locked="interactionLocked"
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
            :interaction-locked="interactionLocked"
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
              :disabled="!canSubmit || interactionLocked"
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
</style>
