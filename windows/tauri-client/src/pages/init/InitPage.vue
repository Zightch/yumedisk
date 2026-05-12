<script setup lang="ts">
import { Loading } from "@element-plus/icons-vue";

defineProps<{
  loading: boolean;
  errorText: string | null;
}>();

defineEmits<{
  retry: [];
}>();
</script>

<template>
  <el-container style="min-height: 100vh">
    <el-main style="display: flex; align-items: center; justify-content: center">
      <el-row justify="center" style="width: 100%">
        <el-col :xs="24" :sm="20" :md="14" :lg="10">
          <el-card shadow="never">
            <el-space direction="vertical" fill size="large">
              <el-space direction="vertical" fill alignment="center" size="small">
                <el-icon v-if="loading" class="is-loading" size="28">
                  <Loading />
                </el-icon>
                <el-text>
                  <strong>正在初始化</strong>
                </el-text>
                <el-text type="info">
                  {{ loading ? "正在打开 BackendRust Session。" : "初始化失败，请重试。" }}
                </el-text>
              </el-space>

              <el-steps :active="0" align-center finish-status="success">
                <el-step title="打开 Session" />
                <el-step title="进入主页" />
              </el-steps>

              <el-alert
                v-if="errorText"
                :title="errorText"
                type="error"
                :closable="false"
                show-icon
              />

              <el-button v-if="!loading" type="primary" @click="$emit('retry')">
                重试
              </el-button>
            </el-space>
          </el-card>
        </el-col>
      </el-row>
    </el-main>
  </el-container>
</template>
