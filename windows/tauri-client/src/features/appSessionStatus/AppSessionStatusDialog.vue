<script setup lang="ts">
import {
  CircleCheckFilled,
  CircleCloseFilled,
  Close,
  Loading,
  RefreshRight,
} from "@element-plus/icons-vue";
import { computed } from "vue";
import {
  formatAppSessionPhaseDescription,
  formatAppSessionPhaseText,
} from "../../entities/appSession/presenter";
import type { AppSessionPhase } from "../../entities/appSession/model";

const props = defineProps<{
  modelValue: boolean;
  appSessionPhase: AppSessionPhase;
  appSessionStatusText: string | null;
}>();

const emit = defineEmits<{
  "update:modelValue": [value: boolean];
  retry: [];
}>();

const dialogVisible = computed({
  get: () => props.modelValue,
  set: (value: boolean) => emit("update:modelValue", value),
});

const isInitializing = computed(() => props.appSessionPhase === "initializing");
const isReady = computed(() => props.appSessionPhase === "ready");
const isFailed = computed(() => props.appSessionPhase === "failed");
const phaseText = computed(() => formatAppSessionPhaseText(props.appSessionPhase));
const descriptionText = computed(() =>
  formatAppSessionPhaseDescription(props.appSessionPhase, props.appSessionStatusText),
);

function handleClose(): void {
  dialogVisible.value = false;
}
</script>

<template>
  <el-dialog
    v-model="dialogVisible"
    class="app-dialog app-dialog--session"
    modal-class="app-dialog-overlay"
    transition="app-dialog-slide-horizontal"
    align-center
    :show-close="false"
  >
    <template #header>
      <div class="app-dialog__header">
        <h3 class="app-dialog__title">会话状态</h3>
        <el-button class="app-dialog__close" text circle aria-label="关闭" @click="handleClose">
          <el-icon>
            <Close />
          </el-icon>
        </el-button>
      </div>
    </template>

    <div class="session-status-dialog">
      <div
        class="session-status-dialog__indicator"
        :class="{
          'is-initializing': isInitializing,
          'is-ready': isReady,
          'is-failed': isFailed,
        }"
      >
        <el-icon v-if="isInitializing">
          <Loading />
        </el-icon>
        <el-icon v-else-if="isReady">
          <CircleCheckFilled />
        </el-icon>
        <el-icon v-else>
          <CircleCloseFilled />
        </el-icon>
      </div>

      <div class="session-status-dialog__content">
        <h4 class="session-status-dialog__state">{{ phaseText }}</h4>
        <p class="session-status-dialog__description">
          {{ descriptionText }}
        </p>
      </div>

      <div v-if="isFailed" class="session-status-dialog__actions">
        <el-button class="app-dialog__button" @click="emit('retry')">
          <el-icon>
            <RefreshRight />
          </el-icon>
          <span>重试</span>
        </el-button>
      </div>
    </div>
  </el-dialog>
</template>
