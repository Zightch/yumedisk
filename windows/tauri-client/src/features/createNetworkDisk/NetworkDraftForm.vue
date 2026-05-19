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
