import { createApp } from "vue";
import ElementPlus from "element-plus";
import "element-plus/dist/index.css";
import "./shared/styles/index.css";
import App from "./app/App.vue";
import { applyTheme, DEFAULT_THEME } from "./shared/theme/theme";

applyTheme(DEFAULT_THEME);

createApp(App).use(ElementPlus).mount("#app");
