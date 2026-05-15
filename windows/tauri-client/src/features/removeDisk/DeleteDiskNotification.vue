<script setup lang="ts">
import { ElButton } from "element-plus";

const props = defineProps<{
  diskName: string;
  onUndo: () => void;
}>();

function handleUndo(event: MouseEvent): void {
  event.preventDefault();
  event.stopPropagation();
  props.onUndo();
}
</script>

<template>
  <div class="remove-disk-notification">
    <p class="remove-disk-notification__text">
      已删除
      <span class="remove-disk-notification__name">{{ diskName }}</span>
    </p>
    <ElButton
      link
      type="primary"
      class="remove-disk-notification__undo"
      @click="handleUndo"
    >
      撤销
    </ElButton>
  </div>
</template>
