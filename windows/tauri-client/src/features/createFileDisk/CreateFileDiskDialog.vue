<script setup lang="ts">
import { ElMessage } from "element-plus";
import { computed, reactive, ref, watch } from "vue";
import type { CreateFileDiskRequest } from "../../entities/disk/model";
import {
  createFileDisk,
  pickRawFilePath,
} from "../../shared/api/diskClient";
import { getErrorMessage } from "../../shared/api/sessionClient";

interface FileDiskFormModel {
  diskName: string;
  filePath: string;
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
const browsing = ref(false);
const errorText = ref<string | null>(null);
const activeTab = ref("pickExisting");
const form = reactive<FileDiskFormModel>({
  diskName: "",
  filePath: "",
  autoConnect: false,
});

watch(
  () => props.modelValue,
  (visible) => {
    if (visible) {
      resetForm();
      activeTab.value = "pickExisting";
      errorText.value = null;
    }
  },
);

function resetForm() {
  form.diskName = "";
  form.filePath = "";
  form.autoConnect = false;
}

function handleCancel() {
  dialogVisible.value = false;
}

function validateRequest(): string | null {
  if (form.diskName.trim().length === 0) {
    return "磁盘名称不能为空";
  }

  if (form.filePath.trim().length === 0) {
    return "文件路径不能为空";
  }

  return null;
}

async function handleBrowse() {
  browsing.value = true;

  try {
    const filePath = await pickRawFilePath();
    if (filePath) {
      form.filePath = filePath;
    }
  } catch (error) {
    errorText.value = getErrorMessage(error);
  } finally {
    browsing.value = false;
  }
}

async function handleSubmit() {
  errorText.value = validateRequest();
  if (errorText.value) {
    return;
  }

  const request: CreateFileDiskRequest = {
    diskName: form.diskName.trim(),
    filePath: form.filePath.trim(),
    autoConnect: form.autoConnect,
  };

  submitting.value = true;

  try {
    await createFileDisk(request);
    ElMessage.success("文件盘已创建");
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
  <el-dialog v-model="dialogVisible" title="新建文件盘" width="520">
    <el-tabs v-model="activeTab">
      <el-tab-pane label="选择现有文件" name="pickExisting">
        <el-form label-position="top">
          <el-form-item label="名称">
            <el-input v-model="form.diskName" placeholder="输入磁盘名称" />
          </el-form-item>

          <el-form-item label="RAW 文件路径">
            <el-input v-model="form.filePath" placeholder="选择现有 RAW 文件">
              <template #append>
                <el-button :loading="browsing" @click="handleBrowse">浏览</el-button>
              </template>
            </el-input>
          </el-form-item>

          <el-form-item label="启动自动连接">
            <el-switch v-model="form.autoConnect" />
          </el-form-item>
        </el-form>
      </el-tab-pane>

      <el-tab-pane label="创建文件" name="createNew">
        <el-empty description="当前阶段暂不支持创建文件" />
      </el-tab-pane>
    </el-tabs>

    <el-alert
      v-if="errorText"
      :title="errorText"
      type="error"
      :closable="false"
      show-icon
    />

    <template #footer>
      <el-space>
        <el-button @click="handleCancel">取消</el-button>
        <el-button
          type="primary"
          :loading="submitting"
          :disabled="activeTab !== 'pickExisting'"
          @click="handleSubmit"
        >
          创建
        </el-button>
      </el-space>
    </template>
  </el-dialog>
</template>
