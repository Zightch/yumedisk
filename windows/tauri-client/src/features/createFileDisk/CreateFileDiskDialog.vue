<script setup lang="ts">
import { ElMessage } from "element-plus";
import { computed, reactive, ref, watch } from "vue";
import type {
  CreateFileDiskRequest,
  CreateFileFormat,
  CreateNewFileDiskRequest,
} from "../../entities/disk/model";
import {
  createFileDisk,
  createNewFileDisk,
  pickNewRawFilePath,
  pickRawFilePath,
} from "../../shared/api/diskClient";
import { getErrorMessage } from "../../shared/api/sessionClient";

interface FileDiskFormModel {
  diskName: string;
  filePath: string;
  autoConnect: boolean;
}

interface NewFileDiskFormModel {
  diskName: string;
  filePath: string;
  capacityMiB: number | null;
  fileFormat: CreateFileFormat;
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
const newFileForm = reactive<NewFileDiskFormModel>({
  diskName: "",
  filePath: "",
  capacityMiB: null,
  fileFormat: "raw",
  autoConnect: false,
});

const createFileFormats: Array<{
  label: string;
  value: CreateFileFormat;
  disabled: boolean;
}> = [
  { label: "RAW", value: "raw", disabled: false },
  { label: "VMDK", value: "vmdk", disabled: true },
  { label: "VHD", value: "vhd", disabled: true },
  { label: "VHDX", value: "vhdx", disabled: true },
  { label: "VDI", value: "vdi", disabled: true },
  { label: "QCOW2", value: "qcow2", disabled: true },
];

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
  newFileForm.diskName = "";
  newFileForm.filePath = "";
  newFileForm.capacityMiB = null;
  newFileForm.fileFormat = "raw";
  newFileForm.autoConnect = false;
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

function validateNewFileRequest(): string | null {
  if (newFileForm.diskName.trim().length === 0) {
    return "磁盘名称不能为空";
  }

  if (newFileForm.filePath.trim().length === 0) {
    return "文件路径不能为空";
  }

  if (
    newFileForm.capacityMiB === null ||
    !Number.isInteger(newFileForm.capacityMiB) ||
    newFileForm.capacityMiB <= 0
  ) {
    return "容量必须是大于 0 的 MiB 整数";
  }

  if (newFileForm.fileFormat !== "raw") {
    return "当前阶段只支持 RAW";
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

async function handleBrowseNewFilePath() {
  browsing.value = true;

  try {
    const filePath = await pickNewRawFilePath();
    if (filePath) {
      newFileForm.filePath = filePath;
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

async function handleCreateNewFileSubmit() {
  errorText.value = validateNewFileRequest();
  if (errorText.value) {
    return;
  }

  const request: CreateNewFileDiskRequest = {
    diskName: newFileForm.diskName.trim(),
    filePath: newFileForm.filePath.trim(),
    capacityMiB: newFileForm.capacityMiB ?? 0,
    fileFormat: newFileForm.fileFormat,
    autoConnect: newFileForm.autoConnect,
  };

  submitting.value = true;

  try {
    await createNewFileDisk(request);
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
        <el-form label-position="top">
          <el-form-item label="名称">
            <el-input v-model="newFileForm.diskName" placeholder="输入磁盘名称" />
          </el-form-item>

          <el-form-item label="文件路径">
            <el-input v-model="newFileForm.filePath" placeholder="输入要创建的 RAW 文件路径">
              <template #append>
                <el-button :loading="browsing" @click="handleBrowseNewFilePath">
                  浏览
                </el-button>
              </template>
            </el-input>
          </el-form-item>

          <el-form-item label="容量（MiB）">
            <el-input-number
              v-model="newFileForm.capacityMiB"
              :min="1"
              :step="1"
              :precision="0"
              controls-position="right"
              style="width: 100%"
            />
          </el-form-item>

          <el-form-item label="文件格式">
            <el-select v-model="newFileForm.fileFormat" style="width: 100%">
              <el-option
                v-for="item in createFileFormats"
                :key="item.value"
                :label="item.label"
                :value="item.value"
                :disabled="item.disabled"
              />
            </el-select>
          </el-form-item>

          <el-form-item label="启动自动连接">
            <el-switch v-model="newFileForm.autoConnect" />
          </el-form-item>
        </el-form>
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
          @click="activeTab === 'pickExisting' ? handleSubmit() : handleCreateNewFileSubmit()"
        >
          创建
        </el-button>
      </el-space>
    </template>
  </el-dialog>
</template>
