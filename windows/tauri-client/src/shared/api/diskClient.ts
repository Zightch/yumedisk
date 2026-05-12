import { invoke } from "@tauri-apps/api/core";
import type { HomeDiskListSnapshot } from "../../entities/disk/model";

export async function queryHomeDiskList(): Promise<HomeDiskListSnapshot> {
  return invoke<HomeDiskListSnapshot>("query_home_disk_list");
}
