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
  <el-dialog v-model="dialogVisible" title="编辑磁盘" width="520">
    <el-form label-position="top">
      <el-form-item label="名称">
        <el-input v-model="form.diskName" placeholder="输入磁盘名称" />
      </el-form-item>

      <el-form-item label="启动自动连接">
        <el-switch v-model="form.autoConnect" />
      </el-form-item>

      <el-alert
        v-if="errorText"
        :title="errorText"
        type="error"
        :closable="false"
        show-icon
      />
    </el-form>

    <template #footer>
      <el-space>
        <el-button @click="handleCancel">取消</el-button>
        <el-button type="primary" :loading="submitting" @click="handleSubmit">
          保存
        </el-button>
      </el-space>
    </template>
  </el-dialog>
</template>
