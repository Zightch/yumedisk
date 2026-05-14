<script setup lang="ts">
import { onBeforeUnmount, onMounted } from "vue";
import HomePage from "../pages/home/HomePage.vue";

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
});

onBeforeUnmount(() => {
  window.removeEventListener("keydown", handleWindowKeydown, true);
});
</script>

<template>
  <div class="app-root">
    <div class="app-window">
      <HomePage />
    </div>
  </div>
</template>
