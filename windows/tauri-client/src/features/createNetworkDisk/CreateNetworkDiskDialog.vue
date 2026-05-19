<script setup lang="ts">
import { ElMessage } from "element-plus";
import { computed, reactive, ref, watch } from "vue";
import type { NetworkDraftItem } from "../../entities/disk/model";
import {
  addNetworkDraftItem,
  createNetworkDraft,
  disposeNetworkDraft,
  removeNetworkDraftItem,
  submitNetworkDraft,
} from "../../shared/api/networkDiskClient";

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

const form = reactive({
  serverAddr: "",
  diskName: "",
  claimCode: "",
});

const testing = ref(false);
const adding = ref(false);
const submitting = ref(false);
const disposingDraft = ref(false);
const removingRemoteDiskId = ref<string | null>(null);
const errorText = ref<string | null>(null);
const draftId = ref<string | null>(null);
const draftServerAddr = ref("");
const draftItems = ref<NetworkDraftItem[]>([]);

const normalizedServerAddr = computed(() => form.serverAddr.trim());
const canEditDraft = computed(
  () =>
    draftId.value !== null &&
    !testing.value &&
    !submitting.value &&
    !adding.value &&
    removingRemoteDiskId.value === null,
);
const canAddItem = computed(
  () =>
    canEditDraft.value &&
    form.diskName.trim().length > 0 &&
    form.claimCode.trim().length > 0,
);
const canSubmit = computed(
  () =>
    draftId.value !== null &&
    draftItems.value.length > 0 &&
    !testing.value &&
    !adding.value &&
    removingRemoteDiskId.value === null &&
    !submitting.value,
);
const connectionStatusText = computed(() => {
  if (testing.value) {
    return "测试连接中";
  }

  if (draftId.value) {
    return `已连接 · ${draftServerAddr.value}`;
  }

  return "尚未测试";
});
const connectionStatusClass = computed(() => {
  if (testing.value) {
    return "network-dialog__status-badge--testing";
  }

  if (draftId.value) {
    return "network-dialog__status-badge--ready";
  }

  return "network-dialog__status-badge--idle";
});

watch(
  () => props.modelValue,
  (visible) => {
    if (visible) {
      resetAllState();
      return;
    }

    void closeDraftSession();
  },
);

watch(normalizedServerAddr, (nextServerAddr) => {
  if (!draftId.value || disposingDraft.value) {
    return;
  }

  if (nextServerAddr === draftServerAddr.value) {
    return;
  }

  void resetDraftForServerChange();
});

function resetAllState() {
  form.serverAddr = "";
  form.diskName = "";
  form.claimCode = "";
  errorText.value = null;
  testing.value = false;
  adding.value = false;
  submitting.value = false;
  disposingDraft.value = false;
  removingRemoteDiskId.value = null;
  draftId.value = null;
  draftServerAddr.value = "";
  draftItems.value = [];
}

function resetDraftInputs() {
  form.diskName = "";
  form.claimCode = "";
  errorText.value = null;
  adding.value = false;
  removingRemoteDiskId.value = null;
  draftItems.value = [];
}

function validateServerAddr(): string | null {
  if (normalizedServerAddr.value.length === 0) {
    return "服务器地址不能为空";
  }

  return null;
}

function validateDraftItem(): string | null {
  if (form.diskName.trim().length === 0) {
    return "磁盘名称不能为空";
  }

  if (form.claimCode.trim().length === 0) {
    return "领盘码不能为空";
  }

  return null;
}

async function closeDraftSession() {
  if (!draftId.value) {
    resetAllState();
    return;
  }

  await disposeCurrentDraft();
  resetAllState();
}

async function resetDraftForServerChange() {
  if (!draftId.value) {
    return;
  }

  await disposeCurrentDraft();
  resetDraftInputs();
}

async function disposeCurrentDraft() {
  if (!draftId.value || disposingDraft.value) {
    return;
  }

  const currentDraftId = draftId.value;
  disposingDraft.value = true;

  try {
    await disposeNetworkDraft({ draftId: currentDraftId });
  } catch {
    ElMessage.error("网络盘草稿清理失败");
  } finally {
    disposingDraft.value = false;
    draftId.value = null;
    draftServerAddr.value = "";
    draftItems.value = [];
  }
}

async function handleTestConnection() {
  const error = validateServerAddr();
  if (error) {
    errorText.value = error;
    ElMessage.error(error);
    return;
  }

  const serverAddr = normalizedServerAddr.value;
  if (draftId.value && draftServerAddr.value === serverAddr) {
    errorText.value = null;
    ElMessage.success("测试连接成功");
    return;
  }

  testing.value = true;
  try {
    if (draftId.value) {
      await disposeCurrentDraft();
    }

    const snapshot = await createNetworkDraft({ serverAddr });
    if (!props.modelValue) {
      return;
    }

    draftId.value = snapshot.draftId;
    draftServerAddr.value = snapshot.serverAddr;
    draftItems.value = snapshot.items;
    errorText.value = null;
    ElMessage.success("测试连接成功");
  } catch {
    errorText.value = "测试连接失败";
    ElMessage.error("测试连接失败");
  } finally {
    testing.value = false;
  }
}

async function handleAddDraftItem() {
  const error = validateDraftItem();
  if (error) {
    errorText.value = error;
    ElMessage.error(error);
    return;
  }

  if (!draftId.value) {
    errorText.value = "请先测试连接";
    ElMessage.error("请先测试连接");
    return;
  }

  adding.value = true;
  try {
    const snapshot = await addNetworkDraftItem({
      draftId: draftId.value,
      diskName: form.diskName.trim(),
      claimCode: form.claimCode.trim(),
    });
    if (!props.modelValue) {
      return;
    }

    draftItems.value = snapshot.items;
    form.diskName = "";
    form.claimCode = "";
    errorText.value = null;
  } catch (error) {
    const message = mapNetworkDraftError(error, "添加网络盘失败");
    errorText.value = message;
    ElMessage.error(message);
  } finally {
    adding.value = false;
  }
}

async function handleRemoveDraftItem(remoteDiskId: string) {
  if (!draftId.value) {
    return;
  }

  removingRemoteDiskId.value = remoteDiskId;
  try {
    const snapshot = await removeNetworkDraftItem({
      draftId: draftId.value,
      remoteDiskId,
    });
    if (!props.modelValue) {
      return;
    }

    draftItems.value = snapshot.items;
    errorText.value = null;
  } catch {
    ElMessage.error("移除网络盘失败");
  } finally {
    removingRemoteDiskId.value = null;
  }
}

async function handleSubmit() {
  if (!draftId.value) {
    errorText.value = "请先测试连接";
    ElMessage.error("请先测试连接");
    return;
  }

  if (draftItems.value.length === 0) {
    errorText.value = "当前没有可提交的网络盘";
    ElMessage.error("当前没有可提交的网络盘");
    return;
  }

  submitting.value = true;
  try {
    await submitNetworkDraft({ draftId: draftId.value });
    draftId.value = null;
    draftServerAddr.value = "";
    draftItems.value = [];
    emit("created");
    ElMessage.success("网络盘已提交");
    dialogVisible.value = false;
  } catch (error) {
    const message = mapNetworkDraftError(error, "提交网络盘失败");
    errorText.value = message;
    ElMessage.error(message);
  } finally {
    submitting.value = false;
  }
}

function handleCancel() {
  dialogVisible.value = false;
}

function mapNetworkDraftError(error: unknown, fallback: string): string {
  const code = getErrorCode(error);
  switch (code) {
    case "network-connect-failed":
      return "测试连接失败";
    case "network-auth-failed":
      return "认证失败";
    case "network-session-open-failed":
      return "会话打开失败";
    case "network-metadata-failed":
      return "元数据获取失败";
    case "network-disk-duplicate":
      return "网络盘已存在";
    case "network-draft-empty":
      return "当前没有可提交的网络盘";
    case "network-draft-not-found":
      return "网络盘草稿不存在";
    case "network-draft-item-not-found":
      return "网络盘草稿项不存在";
    case "network-server-addr-empty":
      return "服务器地址不能为空";
    default:
      return fallback;
  }
}

function getErrorCode(error: unknown): string | null {
  if (error && typeof error === "object") {
    const code = Reflect.get(error, "code");
    if (typeof code === "string" && code.length > 0) {
      return code;
    }
  }

  return null;
}

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
          <section class="network-dialog__section">
            <el-form class="app-dialog-form" label-position="top">
              <el-form-item label="服务器地址">
                <div class="network-dialog__server-row">
                  <el-input
                    v-model="form.serverAddr"
                    placeholder="输入服务器 IP 或地址"
                    :disabled="testing || submitting || adding || removingRemoteDiskId !== null"
                  />
                  <el-button
                    class="network-dialog__test-button"
                    type="primary"
                    :loading="testing"
                    :disabled="submitting || adding || removingRemoteDiskId !== null"
                    @click="handleTestConnection"
                  >
                    测试连接
                  </el-button>
                </div>
              </el-form-item>
            </el-form>

            <div class="network-dialog__status">
              <span class="network-dialog__status-label">连接状态</span>
              <span class="network-dialog__status-badge" :class="connectionStatusClass">
                {{ connectionStatusText }}
              </span>
            </div>
          </section>

          <section class="network-dialog__section">
            <el-form class="app-dialog-form" label-position="top">
              <el-form-item label="磁盘名称">
                <el-input
                  v-model="form.diskName"
                  placeholder="输入磁盘名称"
                  :disabled="!canEditDraft"
                />
              </el-form-item>

              <el-form-item label="领盘码">
                <el-input
                  v-model="form.claimCode"
                  placeholder="输入领盘码"
                  :disabled="!canEditDraft"
                />
              </el-form-item>
            </el-form>

            <div class="network-dialog__actions">
              <el-button
                class="network-dialog__action-button"
                type="primary"
                :loading="adding"
                :disabled="!canAddItem || testing || submitting || removingRemoteDiskId !== null"
                @click="handleAddDraftItem"
              >
                添加
              </el-button>
            </div>
          </section>

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
              <span class="network-dialog__section-count">{{ draftItems.length }}</span>
            </div>

            <el-scrollbar class="network-dialog__scroll">
              <div v-if="draftItems.length === 0" class="network-dialog__empty">
                <el-empty
                  description="先测试连接，再添加网络盘"
                  :image-size="0"
                />
              </div>

              <div v-else class="network-dialog__list">
                <article
                  v-for="item in draftItems"
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
                    @click="handleRemoveDraftItem(item.remoteDiskId)"
                  >
                    删除
                  </el-button>
                </article>
              </div>
            </el-scrollbar>
          </section>

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

.network-dialog__server-row {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  gap: 12px;
  align-items: center;
}

.network-dialog__test-button {
  min-width: 104px;
  border-radius: 8px;
}

.network-dialog__status {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  padding: 12px 14px;
  border: 1px solid rgba(255, 255, 255, 0.08);
  border-radius: 16px;
  background: rgba(255, 255, 255, 0.03);
}

.network-dialog__status-label {
  font-size: 12px;
  letter-spacing: 0.12em;
  text-transform: uppercase;
  opacity: 0.72;
}

.network-dialog__status-badge {
  padding: 6px 12px;
  border-radius: 999px;
  font-size: 12px;
  font-weight: 600;
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

.network-dialog__actions {
  display: flex;
  justify-content: flex-end;
}

.network-dialog__action-button {
  min-width: 104px;
  border-radius: 8px;
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
