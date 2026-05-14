import { invoke } from "@tauri-apps/api/core";
import type { ComponentVersionSnapshot } from "../../entities/appInfo/model";

export async function queryComponentVersions(): Promise<ComponentVersionSnapshot> {
  return invoke<ComponentVersionSnapshot>("query_component_versions");
}
