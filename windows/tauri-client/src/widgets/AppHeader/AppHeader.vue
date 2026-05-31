<script setup lang="ts">
import { computed, ref } from "vue";
import { Loading, Plus, Setting } from "@element-plus/icons-vue";
import { formatAppSessionPhaseText } from "../../entities/appSession/presenter";
import type { AppSessionPhase } from "../../entities/appSession/model";

const props = defineProps<{
  appSessionPhase: AppSessionPhase;
  interactionDisabled: boolean;
}>();

const emit = defineEmits<{
  openAppSessionStatus: [];
  openSettings: [];
  openMemoryCreate: [];
  openFileCreate: [];
  openNetworkCreate: [];
}>();

const addPopoverVisible = ref(false);
const isInitializing = computed(() => props.appSessionPhase === "initializing");
const isReady = computed(() => props.appSessionPhase === "ready");
const isFailed = computed(() => props.appSessionPhase === "failed");
const statusText = computed(() => formatAppSessionPhaseText(props.appSessionPhase));
const canCreateDisk = computed(
  () => props.appSessionPhase === "ready" && !props.interactionDisabled,
);

function handleOpenMemoryCreate() {
  if (!canCreateDisk.value) {
    return;
  }

  addPopoverVisible.value = false;
  emit("openMemoryCreate");
}

function handleOpenFileCreate() {
  if (!canCreateDisk.value) {
    return;
  }

  addPopoverVisible.value = false;
  emit("openFileCreate");
}

function handleOpenNetworkCreate() {
  if (!canCreateDisk.value) {
    return;
  }

  addPopoverVisible.value = false;
  emit("openNetworkCreate");
}
</script>

<template>
  <header class="app-header">
    <div class="app-header__brand">
      <h1 class="app-header__title">YumeDisk</h1>
      <el-button
        class="app-header__settings"
        text
        circle
        aria-label="设置"
        @click="emit('openSettings')"
      >
        <el-icon>
          <Setting />
        </el-icon>
      </el-button>
    </div>

    <div class="app-header__tools">
      <el-button
        class="app-header__status"
        text
        :class="{
          'is-initializing': isInitializing,
          'is-ready': isReady,
          'is-failed': isFailed,
        }"
        @click="emit('openAppSessionStatus')"
      >
        <span v-if="isInitializing" class="app-header__status-spinner">
          <el-icon>
            <Loading />
          </el-icon>
        </span>
        <span v-else class="app-header__status-dot"></span>
        <span class="app-header__status-text">
          {{ statusText }}
        </span>
      </el-button>

      <el-popover
        v-model:visible="addPopoverVisible"
        popper-class="app-header__add-popover"
        placement="bottom-end"
        trigger="click"
        :show-arrow="false"
        :offset="8"
      >
        <template #reference>
          <el-button class="app-header__add" circle aria-label="添加磁盘" :disabled="!canCreateDisk">
            <el-icon>
              <Plus />
            </el-icon>
          </el-button>
        </template>

        <div class="app-header__add-options">
          <el-button
            class="app-header__add-option"
            :disabled="!canCreateDisk"
            @click="handleOpenMemoryCreate"
          >
            <span class="app-header__add-option-icon app-header__add-option-icon--memory">
              M
            </span>
            <span class="app-header__add-option-title">内存盘</span>
          </el-button>

          <el-button
            class="app-header__add-option"
            :disabled="!canCreateDisk"
            @click="handleOpenFileCreate"
          >
            <span class="app-header__add-option-icon app-header__add-option-icon--file">F</span>
            <span class="app-header__add-option-title">文件盘</span>
          </el-button>

          <el-button
            class="app-header__add-option"
            :disabled="!canCreateDisk"
            @click="handleOpenNetworkCreate"
          >
            <span class="app-header__add-option-icon app-header__add-option-icon--network">
              N
            </span>
            <span class="app-header__add-option-title">网络盘</span>
          </el-button>
        </div>
      </el-popover>
    </div>
  </header>
</template>
