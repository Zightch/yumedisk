<script setup lang="ts">
import { ref } from "vue";
import { FolderOpened, Link, Plus, Setting, Wallet } from "@element-plus/icons-vue";

defineProps<{
  sessionReady: boolean;
}>();

const emit = defineEmits<{
  openMemoryCreate: [];
}>();

const addPopoverVisible = ref(false);

function handleOpenMemoryCreate() {
  addPopoverVisible.value = false;
  emit("openMemoryCreate");
}
</script>

<template>
  <el-row justify="space-between" align="middle" style="width: 100%" :gutter="16">
    <el-col :xs="24" :sm="12">
      <el-space wrap>
        <el-text size="large">
          <strong>YumeDisk</strong>
        </el-text>
        <el-button :icon="Setting" circle disabled />
      </el-space>
    </el-col>

    <el-col :xs="24" :sm="12" style="display: flex; justify-content: flex-end">
      <el-space wrap alignment="center">
        <el-tag :type="sessionReady ? 'success' : 'info'" round>
          {{ sessionReady ? "会话正常" : "会话未就绪" }}
        </el-tag>

        <el-popover
          v-model:visible="addPopoverVisible"
          placement="bottom-end"
          trigger="click"
          :width="220"
        >
          <template #reference>
            <el-button :icon="Plus" circle type="primary" />
          </template>

          <el-space direction="vertical" fill>
            <el-button :icon="Wallet" plain @click="handleOpenMemoryCreate">
              内存盘
            </el-button>
            <el-button :icon="FolderOpened" plain disabled>
              文件盘
            </el-button>
            <el-button :icon="Link" plain disabled>
              网络盘
            </el-button>
          </el-space>
        </el-popover>
      </el-space>
    </el-col>
  </el-row>
</template>
