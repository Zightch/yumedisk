<script setup lang="ts">
import { ArrowLeft } from "@element-plus/icons-vue";
import { ElButton, ElCard, ElIcon, ElRadioButton, ElRadioGroup, ElScrollbar } from "element-plus";
import type { AppTheme, ThemeColor, ThemeMode } from "../../shared/theme/theme";

const props = defineProps<{
  modelValue: boolean;
  theme: AppTheme;
}>();

const emit = defineEmits<{
  "update:modelValue": [value: boolean];
  "update:theme": [theme: AppTheme];
}>();

const themeModeOptions: Array<{ value: ThemeMode; label: string }> = [
  { value: "dark", label: "暗色" },
  { value: "light", label: "亮色" },
];

const themeColorOptions: Array<{ value: ThemeColor; label: string }> = [
  { value: "higanRed", label: "彼岸红" },
  { value: "coralOrange", label: "珊瑚橘" },
  { value: "sunsetGold", label: "落日金" },
  { value: "mintGreen", label: "薄荷绿" },
  { value: "moonlightBlue", label: "月光蓝" },
  { value: "twilightPurple", label: "暮光紫" },
];

const recoveryItems = [
  { label: "启动时自动恢复磁盘", value: "开启" },
  { label: "内存盘恢复语义提示", value: "显示" },
  { label: "通知显示时长", value: "3.5 秒" },
];

const defaultCreateItems = [
  { label: "默认一级类型", value: "内存" },
  { label: "默认内存选择策略", value: "自动" },
  { label: "默认文件实现", value: "rawFile" },
];

function handleClose() {
  emit("update:modelValue", false);
}

function handleThemeModeChange(value: string | number | boolean | undefined) {
  if (typeof value !== "string") {
    return;
  }

  emit("update:theme", {
    ...props.theme,
    mode: value as ThemeMode,
  });
}

function handleThemeColorChange(value: string | number | boolean | undefined) {
  if (typeof value !== "string") {
    return;
  }

  emit("update:theme", {
    ...props.theme,
    color: value as ThemeColor,
  });
}
</script>

<template>
  <transition name="settings-page">
    <section v-if="modelValue" class="settings-page">
      <header class="settings-page__header">
        <ElButton
          class="settings-page__back"
          circle
          text
          aria-label="返回主页"
          @click="handleClose"
        >
          <ElIcon>
            <ArrowLeft />
          </ElIcon>
        </ElButton>
        <h2 class="settings-page__title">设置</h2>
      </header>

      <ElScrollbar class="settings-page__scroll">
        <div class="settings-page__content">
          <ElCard class="settings-section" shadow="never">
            <template #header>
              <div class="settings-section__header">
                <h3 class="settings-section__title">主题</h3>
              </div>
            </template>

            <div class="settings-option-group">
              <span class="settings-option-label">模式</span>
              <ElRadioGroup
                class="settings-chip-group"
                :model-value="theme.mode"
                @update:model-value="handleThemeModeChange"
              >
                <ElRadioButton
                  v-for="option in themeModeOptions"
                  :key="option.value"
                  :label="option.value"
                >
                  {{ option.label }}
                </ElRadioButton>
              </ElRadioGroup>
            </div>

            <div class="settings-option-group settings-option-group--stacked">
              <span class="settings-option-label">颜色</span>
              <ElRadioGroup
                class="settings-chip-group settings-chip-group--wrap"
                :model-value="theme.color"
                @update:model-value="handleThemeColorChange"
              >
                <ElRadioButton
                  v-for="option in themeColorOptions"
                  :key="option.value"
                  :label="option.value"
                >
                  {{ option.label }}
                </ElRadioButton>
              </ElRadioGroup>
            </div>
          </ElCard>

          <ElCard class="settings-section" shadow="never">
            <template #header>
              <div class="settings-section__header">
                <h3 class="settings-section__title">恢复策略</h3>
              </div>
            </template>

            <div class="settings-list">
              <div
                v-for="item in recoveryItems"
                :key="item.label"
                class="settings-list-item"
              >
                <span class="settings-list-item__label">{{ item.label }}</span>
                <strong class="settings-list-item__value">{{ item.value }}</strong>
              </div>
            </div>
          </ElCard>

          <ElCard class="settings-section" shadow="never">
            <template #header>
              <div class="settings-section__header">
                <h3 class="settings-section__title">默认创建行为</h3>
              </div>
            </template>

            <div class="settings-list">
              <div
                v-for="item in defaultCreateItems"
                :key="item.label"
                class="settings-list-item"
              >
                <span class="settings-list-item__label">{{ item.label }}</span>
                <strong class="settings-list-item__value">{{ item.value }}</strong>
              </div>
            </div>
          </ElCard>
        </div>
      </ElScrollbar>
    </section>
  </transition>
</template>
