import { invoke } from "@tauri-apps/api/core";
import type {
  ConnectDiskRequest,
  ConnectDiskResponse,
  CreateFileDiskRequest,
  CreateFileDiskResponse,
  CreateMemoryDiskRequest,
  CreateMemoryDiskResponse,
  DisconnectDiskRequest,
  HomeDiskListSnapshot,
  PickRawFilePathResponse,
} from "../../entities/disk/model";

export async function queryHomeDiskList(): Promise<HomeDiskListSnapshot> {
  return invoke<HomeDiskListSnapshot>("query_home_disk_list");
}

export async function createMemoryDisk(
  request: CreateMemoryDiskRequest,
): Promise<CreateMemoryDiskResponse> {
  return invoke<CreateMemoryDiskResponse>("create_memory_disk", { request });
}

export async function pickRawFilePath(): Promise<string | null> {
  const response = await invoke<PickRawFilePathResponse>("pick_raw_file_path");
  return response.filePath;
}

export async function createFileDisk(
  request: CreateFileDiskRequest,
): Promise<CreateFileDiskResponse> {
  return invoke<CreateFileDiskResponse>("create_file_disk", { request });
}

export async function connectDisk(
  request: ConnectDiskRequest,
): Promise<ConnectDiskResponse> {
  return invoke<ConnectDiskResponse>("connect_disk", { request });
}

export async function disconnectDisk(
  request: DisconnectDiskRequest,
): Promise<void> {
  return invoke<void>("disconnect_disk", { request });
}
