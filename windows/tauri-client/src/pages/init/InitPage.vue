<script setup lang="ts">
import { CircleCloseFilled, Loading } from "@element-plus/icons-vue";

defineProps<{
  loading: boolean;
  errorText: string | null;
}>();

defineEmits<{
  retry: [];
}>();
</script>

<template>
  <el-container class="app-shell init-page">
    <el-main class="init-page__main">
      <div class="init-page__center">
        <el-card class="init-page__panel" shadow="never">
          <div class="init-page__content">
            <div class="init-page__spinner">
              <el-icon v-if="loading" class="is-loading">
                <Loading />
              </el-icon>
              <el-icon v-else>
                <CircleCloseFilled />
              </el-icon>
            </div>

            <h1 class="init-page__title">正在初始化</h1>
            <p class="init-page__description">
              {{ loading ? "正在打开 BackendRust Session。" : "初始化失败，请重试。" }}
            </p>

            <el-alert
              v-if="errorText"
              class="init-page__alert"
              :title="errorText"
              type="error"
              :closable="false"
              show-icon
            />

            <div v-if="!loading" class="init-page__actions">
              <el-button class="init-page__retry" type="primary" @click="$emit('retry')">
                重试
              </el-button>
            </div>
          </div>
        </el-card>
      </div>
    </el-main>
  </el-container>
</template>
