<script setup lang="ts">
import { Delete, Edit } from "@element-plus/icons-vue";
import { computed } from "vue";
import type { HomeDiskListItem } from "../../entities/disk/model";
import {
  formatDiskDetailText,
  formatDiskSummaryText,
} from "../../entities/disk/presenter";

const props = defineProps<{
  disk: HomeDiskListItem;
  actionLoading: boolean;
}>();

const emit = defineEmits<{
  connect: [diskId: string];
  disconnect: [diskId: string];
  delete: [diskId: string];
  edit: [diskId: string];
}>();

const detailText = computed(() => formatDiskDetailText(props.disk));
const summaryText = computed(() => formatDiskSummaryText(props.disk));
const isConnected = computed(() => props.disk.status === "connected");
const isInvalid = computed(() => props.disk.status === "invalid");
const avatarText = computed(() => (props.disk.media.kind === "memory" ? "M" : "F"));
const avatarClassName = computed(() =>
  props.disk.media.kind === "memory" ? "disk-card__avatar--memory" : "disk-card__avatar--file",
);

const statusText = computed(() => {
  if (props.disk.status === "connected") {
    return "已连接";
  }

  if (props.disk.status === "invalid") {
    return "无效";
  }

  return "未连接";
});

const statusClassName = computed(() => {
  if (props.disk.status === "connected") {
    return "disk-card__status--connected";
  }

  if (props.disk.status === "invalid") {
    return "disk-card__status--invalid";
  }

  return "disk-card__status--disconnected";
});

const primaryActionText = computed(() => (isConnected.value ? "断开" : "连接"));
const primaryActionClassName = computed(() =>
  isConnected.value ? "disk-card__primary-action--disconnect" : "disk-card__primary-action--connect",
);

function handlePrimaryAction(): void {
  if (props.actionLoading) {
    return;
  }

  if (isConnected.value) {
    emit("disconnect", props.disk.diskId);
    return;
  }

  emit("connect", props.disk.diskId);
}

function handleEdit(): void {
  if (props.actionLoading) {
    return;
  }

  emit("edit", props.disk.diskId);
}

function handleDelete(): void {
  if (props.actionLoading) {
    return;
  }

  emit("delete", props.disk.diskId);
}
</script>

<template>
  <article class="disk-card" :class="{ 'is-connected': isConnected }">
    <div class="disk-card__badges">
      <span v-if="disk.autoConnect" class="disk-card__status disk-card__status--auto">
        自动连接
      </span>

      <el-tooltip
        :disabled="!isInvalid || !disk.invalidReason"
        :content="disk.invalidReason ?? ''"
        placement="top"
      >
        <span class="disk-card__status" :class="statusClassName">
          {{ statusText }}
        </span>
      </el-tooltip>
    </div>

    <span class="disk-card__avatar" :class="avatarClassName">
      {{ avatarText }}
    </span>

    <div class="disk-card__main">
      <div class="disk-card__topline">
        <h3 class="disk-card__title">{{ disk.diskName }}</h3>
      </div>

      <p class="disk-card__summary">{{ summaryText }}</p>
      <p v-if="detailText" class="disk-card__detail">{{ detailText }}</p>
    </div>

    <div class="disk-card__side">
      <div class="disk-card__actions">
        <el-button
          class="disk-card__primary-action"
          :class="primaryActionClassName"
          size="small"
          :loading="actionLoading"
          :disabled="isInvalid"
          @click="handlePrimaryAction"
        >
          {{ primaryActionText }}
        </el-button>

        <el-button
          class="disk-card__icon-action"
          aria-label="编辑"
          :disabled="actionLoading"
          @click="handleEdit"
        >
          <el-icon>
            <Edit />
          </el-icon>
        </el-button>

        <el-button
          class="disk-card__icon-action disk-card__icon-action--danger"
          aria-label="删除"
          :disabled="actionLoading"
          @click="handleDelete"
        >
          <el-icon>
            <Delete />
          </el-icon>
        </el-button>
      </div>
    </div>
  </article>
</template>
