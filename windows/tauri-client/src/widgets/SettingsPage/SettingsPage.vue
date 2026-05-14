<script setup lang="ts">
import { ArrowLeft } from "@element-plus/icons-vue";
import {
  ElButton,
  ElCard,
  ElIcon,
  ElRadioButton,
  ElRadioGroup,
  ElScrollbar,
  ElTabPane,
  ElTabs,
} from "element-plus";
import { computed, ref, watch } from "vue";
import type { ComponentVersionSnapshot } from "../../entities/appInfo/model";
import { queryComponentVersions } from "../../shared/api/appInfoClient";
import type { AppTheme, ThemeColor, ThemeMode } from "../../shared/theme/theme";

const props = defineProps<{
  modelValue: boolean;
  theme: AppTheme;
}>();

const emit = defineEmits<{
  "update:modelValue": [value: boolean];
  "update:theme": [theme: AppTheme];
}>();

type SettingsTabName = "general" | "about";

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

const activeTab = ref<SettingsTabName>("general");
const componentVersions = ref<ComponentVersionSnapshot | null>(null);

const aboutVersionItems = computed(() => [
  {
    label: "AppKernel",
    value: componentVersions.value?.appkernelVersionText?.trim() || "未获取",
  },
  {
    label: "KMDF",
    value: componentVersions.value?.kmdfVersionText?.trim() || "未获取",
  },
  {
    label: "SCSI",
    value: componentVersions.value?.scsiVersionText?.trim() || "未获取",
  },
]);

watch(
  () => props.modelValue,
  async (visible) => {
    if (visible) {
      activeTab.value = "general";
      try {
        componentVersions.value = await queryComponentVersions();
      } catch {
        componentVersions.value = null;
      }
    }
  },
);

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
          <ElTabs v-model="activeTab" class="settings-tabs">
            <ElTabPane label="通用" name="general">
              <div class="settings-tab-panel">
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
              </div>
            </ElTabPane>

            <ElTabPane label="关于" name="about">
              <div class="settings-tab-panel">
                <ElCard class="settings-section" shadow="never">
                  <template #header>
                    <div class="settings-section__header">
                      <h3 class="settings-section__title">关于</h3>
                    </div>
                  </template>

                  <div class="settings-about">
                    <p class="settings-about__copyright">Copyright © 2026 Zightch</p>

                    <div class="settings-list">
                      <div
                        v-for="item in aboutVersionItems"
                        :key="item.label"
                        class="settings-list-item"
                      >
                        <span class="settings-list-item__label">{{ item.label }}</span>
                        <span class="settings-list-item__value">{{ item.value }}</span>
                      </div>
                    </div>
                  </div>
                </ElCard>
              </div>
            </ElTabPane>
          </ElTabs>
        </div>
      </ElScrollbar>
    </section>
  </transition>
</template>
