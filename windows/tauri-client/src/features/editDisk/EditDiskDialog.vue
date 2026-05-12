<script setup lang="ts">
import { ElMessage } from "element-plus";
import { computed, reactive, ref, watch } from "vue";
import type { HomeDiskListItem, UpdateDiskRequest } from "../../entities/disk/model";
import { updateDisk } from "../../shared/api/diskClient";
import { getErrorMessage } from "../../shared/api/sessionClient";

interface EditDiskFormModel {
  diskName: string;
  autoConnect: boolean;
}

const props = defineProps<{
  modelValue: boolean;
  disk: HomeDiskListItem | null;
}>();

const emit = defineEmits<{
  "update:modelValue": [value: boolean];
  updated: [];
}>();

const dialogVisible = computed({
  get: () => props.modelValue,
  set: (value: boolean) => emit("update:modelValue", value),
});

const submitting = ref(false);
const errorText = ref<string | null>(null);
const form = reactive<EditDiskFormModel>({
  diskName: "",
  autoConnect: false,
});

watch(
  () => [props.modelValue, props.disk] as const,
  ([visible, disk]) => {
    if (!visible || disk === null) {
      return;
    }

    form.diskName = disk.diskName;
    form.autoConnect = disk.autoConnect;
    errorText.value = null;
  },
  { immediate: true },
);

function handleCancel() {
  dialogVisible.value = false;
}

function validateRequest(): string | null {
  if (props.disk === null) {
    return "磁盘不存在";
  }

  if (form.diskName.trim().length === 0) {
    return "磁盘名称不能为空";
  }

  return null;
}

async function handleSubmit() {
  errorText.value = validateRequest();
  if (errorText.value || props.disk === null) {
    return;
  }

  const request: UpdateDiskRequest = {
    diskId: props.disk.diskId,
    diskName: form.diskName.trim(),
    autoConnect: form.autoConnect,
  };

  submitting.value = true;

  try {
    await updateDisk(request);
    ElMessage.success("磁盘配置已更新");
    emit("updated");
    dialogVisible.value = false;
  } catch (error) {
    errorText.value = getErrorMessage(error);
  } finally {
    submitting.value = false;
  }
}
</script>

<template>
  <el-dialog
    v-model="dialogVisible"
    class="app-dialog app-dialog--edit"
    modal-class="app-dialog-overlay"
    width="392px"
    align-center
    :show-close="false"
  >
    <template #header>
      <div class="app-dialog__header">
        <h3 class="app-dialog__title">编辑磁盘</h3>
        <button class="app-dialog__close" type="button" aria-label="关闭" @click="handleCancel">
          ×
        </button>
      </div>
    </template>

    <div class="app-dialog__content">
      <el-form class="app-dialog-form" label-position="top">
        <el-form-item label="名称">
          <el-input v-model="form.diskName" placeholder="输入磁盘名称" />
        </el-form-item>

        <el-form-item class="app-dialog-form__switch" label="启动自动连接">
          <el-switch v-model="form.autoConnect" />
        </el-form-item>
      </el-form>

      <el-alert
        v-if="errorText"
        class="app-dialog__alert"
        :title="errorText"
        type="error"
        :closable="false"
        show-icon
      />
    </div>

    <template #footer>
      <div class="app-dialog__footer">
        <el-button class="app-dialog__button app-dialog__button--secondary" @click="handleCancel">
          取消
        </el-button>
        <el-button
          class="app-dialog__button"
          type="primary"
          :loading="submitting"
          @click="handleSubmit"
        >
          保存
        </el-button>
      </div>
    </template>
  </el-dialog>
</template>
