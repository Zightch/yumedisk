import { invoke } from "@tauri-apps/api/core";
import type {
  MountDiskRequest,
  MountDiskResponse,
  CreateFileDiskRequest,
  CreateFileDiskResponse,
  CreateNewFileDiskRequest,
  CreateNewFileDiskResponse,
  CreateMemoryDiskRequest,
  CreateMemoryDiskResponse,
  DeleteDiskRequest,
  DeleteDiskResponse,
  UndoDeleteDiskRequest,
  CommitDeletedDiskRequest,
  EjectDiskRequest,
  HomeDiskListSnapshot,
  PickRawFilePathResponse,
  RuntimeRescanStartResponse,
  RuntimeRescanState,
  UpdateDiskRequest,
} from "../../entities/disk/model";

export async function queryHomeDiskList(): Promise<HomeDiskListSnapshot> {
  return invoke<HomeDiskListSnapshot>("query_home_disk_list");
}

export async function rescanRuntimeDisks(): Promise<RuntimeRescanStartResponse> {
  return invoke<RuntimeRescanStartResponse>("rescan_runtime_disks");
}

export async function queryRuntimeRescanState(): Promise<RuntimeRescanState> {
  return invoke<RuntimeRescanState>("query_runtime_rescan_state");
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

export async function pickNewRawFilePath(): Promise<string | null> {
  const response = await invoke<PickRawFilePathResponse>("pick_new_raw_file_path");
  return response.filePath;
}

export async function createFileDisk(
  request: CreateFileDiskRequest,
): Promise<CreateFileDiskResponse> {
  return invoke<CreateFileDiskResponse>("create_file_disk", { request });
}

export async function createNewFileDisk(
  request: CreateNewFileDiskRequest,
): Promise<CreateNewFileDiskResponse> {
  return invoke<CreateNewFileDiskResponse>("create_new_file_disk", { request });
}

export async function mountDisk(
  request: MountDiskRequest,
): Promise<MountDiskResponse> {
  return invoke<MountDiskResponse>("mount_disk", { request });
}

export async function ejectDisk(
  request: EjectDiskRequest,
): Promise<void> {
  return invoke<void>("eject_disk", { request });
}

export async function deleteDisk(
  request: DeleteDiskRequest,
): Promise<DeleteDiskResponse> {
  return invoke<DeleteDiskResponse>("delete_disk", { request });
}

export async function undoDeleteDisk(
  request: UndoDeleteDiskRequest,
): Promise<void> {
  return invoke<void>("undo_delete_disk", { request });
}

export async function commitDeletedDisk(
  request: CommitDeletedDiskRequest,
): Promise<void> {
  return invoke<void>("commit_deleted_disk", { request });
}

export async function updateDisk(
  request: UpdateDiskRequest,
): Promise<void> {
  return invoke<void>("update_disk", { request });
}
