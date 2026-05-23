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
import { getErrorMessage } from "../../shared/api/appSessionClient";

interface FileDiskFormModel {
  diskName: string;
  filePath: string;
  autoMount: boolean;
  configuredReadOnly: boolean;
}

interface NewFileDiskFormModel {
  diskName: string;
  filePath: string;
  capacityMiB: number | null;
  fileFormat: CreateFileFormat;
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
const browsing = ref(false);
const errorText = ref<string | null>(null);
const activeTab = ref("pickExisting");
const form = reactive<FileDiskFormModel>({
  diskName: "",
  filePath: "",
  autoMount: false,
  configuredReadOnly: false,
});
const newFileForm = reactive<NewFileDiskFormModel>({
  diskName: "",
  filePath: "",
  capacityMiB: null,
  fileFormat: "raw",
  autoMount: false,
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

const isPickExistingTab = computed(() => activeTab.value === "pickExisting");

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
  form.autoMount = false;
  form.configuredReadOnly = false;
  newFileForm.diskName = "";
  newFileForm.filePath = "";
  newFileForm.capacityMiB = null;
  newFileForm.fileFormat = "raw";
  newFileForm.autoMount = false;
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
    autoMount: form.autoMount,
    configuredReadOnly: form.configuredReadOnly,
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
    autoMount: newFileForm.autoMount,
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
  <el-dialog
    v-model="dialogVisible"
    class="app-dialog app-dialog--file"
    modal-class="app-dialog-overlay"
    transition="app-dialog-slide-horizontal"
    width="456px"
    align-center
    :show-close="false"
  >
    <template #header>
      <div class="app-dialog__header">
        <h3 class="app-dialog__title">创建文件盘</h3>
        <el-button class="app-dialog__close" text circle aria-label="关闭" @click="handleCancel">
          ×
        </el-button>
      </div>
    </template>

    <div class="app-dialog__body app-dialog__body--tabbed">
      <el-tabs v-model="activeTab" class="app-dialog-tabs">
        <el-tab-pane label="选择现有文件" name="pickExisting" />
        <el-tab-pane label="创建文件" name="createNew" />
      </el-tabs>

      <div class="app-dialog__viewport app-dialog__viewport--tabbed">
        <div v-if="isPickExistingTab" class="app-dialog__content app-dialog__content--tabbed">
          <el-form class="app-dialog-form" label-position="top">
            <el-form-item label="名称">
              <el-input v-model="form.diskName" placeholder="输入磁盘名称" />
            </el-form-item>

            <el-form-item label="文件路径">
              <div class="app-dialog-path-field">
                <el-input v-model="form.filePath" placeholder="选择现有文件" />
                <el-button
                  class="app-dialog-path-field__browse"
                  :loading="browsing"
                  @click="handleBrowse"
                >
                  浏览
                </el-button>
              </div>
            </el-form-item>

            <el-form-item class="app-dialog-form__switch" label="启动自动挂载">
              <el-switch v-model="form.autoMount" />
            </el-form-item>

            <el-form-item class="app-dialog-form__switch" label="只读">
              <el-switch v-model="form.configuredReadOnly" />
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

        <div v-else class="app-dialog__content app-dialog__content--tabbed">
          <el-form class="app-dialog-form" label-position="top">
            <el-form-item label="名称">
              <el-input v-model="newFileForm.diskName" placeholder="输入磁盘名称" />
            </el-form-item>

            <el-form-item label="文件路径">
              <div class="app-dialog-path-field">
                <el-input v-model="newFileForm.filePath" placeholder="输入要创建的文件路径" />
                <el-button
                  class="app-dialog-path-field__browse"
                  :loading="browsing"
                  @click="handleBrowseNewFilePath"
                >
                  浏览
                </el-button>
              </div>
            </el-form-item>

            <el-form-item label="容量 (MiB)">
              <el-input-number
                v-model="newFileForm.capacityMiB"
                :min="1"
                :step="1"
                :precision="0"
                controls-position="right"
              />
            </el-form-item>

            <el-form-item label="文件格式">
              <el-radio-group v-model="newFileForm.fileFormat" class="app-dialog-chips">
                <el-radio-button
                  v-for="item in createFileFormats"
                  :key="item.value"
                  :value="item.value"
                  :disabled="item.disabled"
                >
                  {{ item.label }}
                </el-radio-button>
              </el-radio-group>
            </el-form-item>

            <el-form-item class="app-dialog-form__switch" label="启动自动挂载">
              <el-switch v-model="newFileForm.autoMount" />
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
              @click="handleCreateNewFileSubmit"
            >
              创建
            </el-button>
          </div>
        </div>
      </div>
    </div>
  </el-dialog>
</template>
