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
  interactionDisabled: boolean;
}>();

const emit = defineEmits<{
  mount: [localDiskId: string];
  eject: [localDiskId: string];
  delete: [localDiskId: string];
  edit: [localDiskId: string];
}>();

const detailText = computed(() => formatDiskDetailText(props.disk));
const summaryText = computed(() => formatDiskSummaryText(props.disk));
const isMounted = computed(() => props.disk.status === "mounted");
const isInvalid = computed(() => props.disk.status === "invalid");
const avatarText = computed(() => {
  if (props.disk.media.kind === "memory") {
    return "M";
  }

  if (props.disk.media.kind === "network") {
    return "N";
  }

  return "F";
});
const avatarClassName = computed(() =>
  props.disk.media.kind === "memory"
    ? "disk-card__avatar--memory"
    : props.disk.media.kind === "network"
      ? "disk-card__avatar--network"
      : "disk-card__avatar--file",
);

const statusText = computed(() => {
  if (props.disk.status === "mounted") {
    return "已挂载";
  }

  if (props.disk.status === "invalid") {
    return "无效";
  }

  return "未挂载";
});

const statusClassName = computed(() => {
  if (props.disk.status === "mounted") {
    return "disk-card__status--mounted";
  }

  if (props.disk.status === "invalid") {
    return "disk-card__status--invalid";
  }

  return "disk-card__status--unmounted";
});

const primaryActionText = computed(() => (isMounted.value ? "拔出" : "挂载"));
const primaryActionClassName = computed(() =>
  isMounted.value ? "disk-card__primary-action--eject" : "disk-card__primary-action--mount",
);

function handlePrimaryAction(): void {
  if (props.actionLoading || props.interactionDisabled) {
    return;
  }

  if (isMounted.value) {
    emit("eject", props.disk.localDiskId);
    return;
  }

  emit("mount", props.disk.localDiskId);
}

function handleEdit(): void {
  if (props.actionLoading || props.interactionDisabled) {
    return;
  }

  emit("edit", props.disk.localDiskId);
}

function handleDelete(): void {
  if (props.actionLoading || props.interactionDisabled) {
    return;
  }

  emit("delete", props.disk.localDiskId);
}
</script>

<template>
  <article class="disk-card" :class="{ 'is-mounted': isMounted }">
    <div class="disk-card__badges">
      <span v-if="disk.autoMount" class="disk-card__status disk-card__status--auto">
        自动挂载
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
          :disabled="isInvalid || interactionDisabled"
          @click="handlePrimaryAction"
        >
          {{ primaryActionText }}
        </el-button>

        <el-button
          class="disk-card__icon-action"
          aria-label="编辑"
          :disabled="actionLoading || interactionDisabled"
          @click="handleEdit"
        >
          <el-icon>
            <Edit />
          </el-icon>
        </el-button>

        <el-button
          class="disk-card__icon-action disk-card__icon-action--danger"
          aria-label="删除"
          :disabled="actionLoading || interactionDisabled"
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
