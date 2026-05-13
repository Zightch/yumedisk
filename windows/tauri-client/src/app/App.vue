<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref } from "vue";
import HomePage from "../pages/home/HomePage.vue";
import InitPage from "../pages/init/InitPage.vue";
import type { SessionSnapshot } from "../entities/session/model";
import { getErrorMessage, initializeClient } from "../shared/api/sessionClient";

type BootPhase = "initializing" | "ready" | "failed";

const phase = ref<BootPhase>("initializing");
const errorText = ref<string | null>(null);
const sessionSnapshot = ref<SessionSnapshot | null>(null);

async function startInitialization() {
  phase.value = "initializing";
  errorText.value = null;

  try {
    const response = await initializeClient();
    sessionSnapshot.value = response.session;
    phase.value = "ready";
  } catch (error) {
    sessionSnapshot.value = null;
    errorText.value = getErrorMessage(error);
    phase.value = "failed";
  }
}

function handleRetry() {
  void startInitialization();
}

function handleWindowKeydown(event: KeyboardEvent) {
  const isRefreshKey = event.key === "F5";
  const isReloadShortcut =
    (event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "r";

  if (!isRefreshKey && !isReloadShortcut) {
    return;
  }

  event.preventDefault();
  event.stopPropagation();
}

onMounted(() => {
  window.addEventListener("keydown", handleWindowKeydown, true);
  void startInitialization();
});

onBeforeUnmount(() => {
  window.removeEventListener("keydown", handleWindowKeydown, true);
});
</script>

<template>
  <div class="app-root">
    <div class="app-window">
      <InitPage
        v-if="phase !== 'ready'"
        :loading="phase === 'initializing'"
        :error-text="errorText"
        @retry="handleRetry"
      />
      <HomePage v-else :session-ready="sessionSnapshot?.ready ?? false" />
    </div>
  </div>
</template>
