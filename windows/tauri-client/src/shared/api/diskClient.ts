import { invoke } from "@tauri-apps/api/core";
import type {
  CreateMemoryDiskRequest,
  CreateMemoryDiskResponse,
  HomeDiskListSnapshot,
} from "../../entities/disk/model";

export async function queryHomeDiskList(): Promise<HomeDiskListSnapshot> {
  return invoke<HomeDiskListSnapshot>("query_home_disk_list");
}

export async function createMemoryDisk(
  request: CreateMemoryDiskRequest,
): Promise<CreateMemoryDiskResponse> {
  return invoke<CreateMemoryDiskResponse>("create_memory_disk", { request });
}
