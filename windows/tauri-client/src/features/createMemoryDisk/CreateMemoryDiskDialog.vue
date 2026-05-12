<script setup lang="ts">
import { ElMessage } from "element-plus";
import { computed, reactive, ref, watch } from "vue";
import type {
  CreateMemoryDiskRequest,
  MemoryCreateKind,
} from "../../entities/disk/model";
import { createMemoryDisk } from "../../shared/api/diskClient";
import { getErrorMessage } from "../../shared/api/sessionClient";

interface MemoryDiskFormModel {
  diskName: string;
  capacityMiB: number | null;
  requestedMemoryKind: MemoryCreateKind;
  autoConnect: boolean;
}

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

const submitting = ref(false);
const errorText = ref<string | null>(null);
const form = reactive<MemoryDiskFormModel>({
  diskName: "",
  capacityMiB: 64,
  requestedMemoryKind: "auto",
  autoConnect: false,
});

watch(
  () => props.modelValue,
  (visible) => {
    if (visible) {
      resetForm();
      errorText.value = null;
    }
  },
);

function resetForm() {
  form.diskName = "";
  form.capacityMiB = 64;
  form.requestedMemoryKind = "auto";
  form.autoConnect = false;
}

function handleCancel() {
  dialogVisible.value = false;
}

function validateRequest(): string | null {
  if (form.diskName.trim().length === 0) {
    return "磁盘名称不能为空";
  }

  if (
    form.capacityMiB === null ||
    !Number.isInteger(form.capacityMiB) ||
    form.capacityMiB <= 0
  ) {
    return "容量必须是大于 0 的 MiB 整数";
  }

  return null;
}

async function handleSubmit() {
  errorText.value = validateRequest();
  if (errorText.value) {
    return;
  }

  const request: CreateMemoryDiskRequest = {
    diskName: form.diskName.trim(),
    capacityMiB: form.capacityMiB ?? 0,
    requestedMemoryKind: form.requestedMemoryKind,
    autoConnect: form.autoConnect,
  };

  submitting.value = true;

  try {
    await createMemoryDisk(request);
    ElMessage.success("内存盘已创建");
    emit("created");
    dialogVisible.value = false;
  } catch (error) {
    errorText.value = getErrorMessage(error);
  } finally {
    submitting.value = false;
  }
}
</script>

<template>
  <el-dialog v-model="dialogVisible" title="新建内存盘" width="520">
    <el-form label-position="top">
      <el-form-item label="名称">
        <el-input v-model="form.diskName" placeholder="输入磁盘名称" />
      </el-form-item>

      <el-form-item label="容量 (MiB)">
        <el-input-number
          v-model="form.capacityMiB"
          :min="1"
          :step="1"
          style="width: 100%"
        />
      </el-form-item>

      <el-form-item label="介质选择">
        <el-select
          v-model="form.requestedMemoryKind"
          placeholder="选择介质策略"
          style="width: 100%"
        >
          <el-option label="自动" value="auto" />
          <el-option label="稠密" value="denseMem" />
          <el-option label="稀疏" value="sparseMem" />
        </el-select>
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
          创建
        </el-button>
      </el-space>
    </template>
  </el-dialog>
</template>
