import { createApp } from "vue";
import ElementPlus from "element-plus";
import "element-plus/dist/index.css";
import { getCurrentWindow } from "@tauri-apps/api/window";
import App from "./app/App.vue";

const app = createApp(App);
app.use(ElementPlus);
app.mount("#app");

requestAnimationFrame(() => {
  requestAnimationFrame(() => {
    void getCurrentWindow().show();
  });
});
