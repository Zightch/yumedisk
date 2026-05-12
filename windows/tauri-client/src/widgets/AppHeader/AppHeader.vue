<script setup lang="ts">
import { ref } from "vue";
import { Plus, Setting } from "@element-plus/icons-vue";

const props = defineProps<{
  sessionReady: boolean;
}>();

const emit = defineEmits<{
  openSettings: [];
  openMemoryCreate: [];
  openFileCreate: [];
}>();

const addPopoverVisible = ref(false);

function handleOpenMemoryCreate() {
  addPopoverVisible.value = false;
  emit("openMemoryCreate");
}

function handleOpenFileCreate() {
  addPopoverVisible.value = false;
  emit("openFileCreate");
}
</script>

<template>
  <header class="app-header">
    <div class="app-header__brand">
      <h1 class="app-header__title">YumeDisk</h1>
      <button
        class="app-header__settings"
        type="button"
        aria-label="设置"
        @click="emit('openSettings')"
      >
        <el-icon>
          <Setting />
        </el-icon>
      </button>
    </div>

    <div class="app-header__tools">
      <div class="app-header__status" :class="{ 'is-ready': props.sessionReady }">
        <span class="app-header__status-dot"></span>
        <span class="app-header__status-text">
          {{ props.sessionReady ? "会话正常" : "会话未就绪" }}
        </span>
      </div>

      <el-popover
        v-model:visible="addPopoverVisible"
        popper-class="app-header__add-popover"
        placement="bottom-end"
        trigger="click"
        :show-arrow="false"
        :width="248"
        :offset="8"
      >
        <template #reference>
          <button class="app-header__add" type="button" aria-label="添加磁盘">
            <el-icon>
              <Plus />
            </el-icon>
          </button>
        </template>

        <div class="app-header__add-options">
          <button class="app-header__add-option" type="button" @click="handleOpenMemoryCreate">
            <span class="app-header__add-option-icon app-header__add-option-icon--memory">
              M
            </span>
            <span class="app-header__add-option-title">内存盘</span>
          </button>

          <button class="app-header__add-option" type="button" @click="handleOpenFileCreate">
            <span class="app-header__add-option-icon app-header__add-option-icon--file">F</span>
            <span class="app-header__add-option-title">文件盘</span>
          </button>

          <button class="app-header__add-option" type="button" disabled>
            <span class="app-header__add-option-icon app-header__add-option-icon--network">
              N
            </span>
            <span class="app-header__add-option-title">网络盘</span>
          </button>
        </div>
      </el-popover>
    </div>
  </header>
</template>
