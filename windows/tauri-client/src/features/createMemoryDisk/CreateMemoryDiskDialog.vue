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
  autoMount: boolean;
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
  autoMount: false,
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
  form.autoMount = false;
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
    autoMount: form.autoMount,
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
  <el-dialog
    v-model="dialogVisible"
    class="app-dialog app-dialog--memory"
    modal-class="app-dialog-overlay"
    transition="app-dialog-slide-horizontal"
    width="392px"
    align-center
    :show-close="false"
  >
    <template #header>
      <div class="app-dialog__header">
        <h3 class="app-dialog__title">创建内存盘</h3>
        <el-button class="app-dialog__close" text circle aria-label="关闭" @click="handleCancel">
          ×
        </el-button>
      </div>
    </template>

    <div class="app-dialog__body">
      <div class="app-dialog__viewport">
        <div class="app-dialog__content">
          <el-form class="app-dialog-form" label-position="top">
            <el-form-item label="名称">
              <el-input v-model="form.diskName" placeholder="输入磁盘名称" />
            </el-form-item>

            <el-form-item label="容量 (MiB)">
              <el-input-number
                v-model="form.capacityMiB"
                :min="1"
                :step="1"
                :precision="0"
                controls-position="right"
              />
            </el-form-item>

            <el-form-item label="介质选择">
              <el-radio-group v-model="form.requestedMemoryKind" class="app-dialog-chips">
                <el-radio-button value="auto">自动</el-radio-button>
                <el-radio-button value="denseMem">稠密</el-radio-button>
                <el-radio-button value="sparseMem">稀疏</el-radio-button>
              </el-radio-group>
            </el-form-item>

            <el-form-item class="app-dialog-form__switch" label="启动自动挂载">
              <el-switch v-model="form.autoMount" />
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
              @click="handleSubmit"
            >
              创建
            </el-button>
          </div>
        </div>
      </div>
    </div>
  </el-dialog>
</template>
