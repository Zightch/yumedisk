export type MemoryMediaKind = "denseMem" | "sparseMem";
export type FileMediaKind = "rawFile";
export type CreateFileFormat = "raw" | "vmdk" | "vhd" | "vhdx" | "vdi" | "qcow2";
export type MemoryCreateKind = "auto" | MemoryMediaKind;
export type DiskStatus = "unmounted" | "mounted" | "invalid";

export type HomeDiskMedia =
  | {
      kind: "memory";
      memoryKind: MemoryMediaKind;
      capacityBytes: number;
    }
  | {
      kind: "file";
      fileKind: FileMediaKind;
      filePath: string;
      capacityBytes: number;
    };

export interface HomeDiskListItem {
  diskId: string;
  diskName: string;
  autoMount: boolean;
  readOnly: boolean;
  status: DiskStatus;
  invalidReason: string | null;
  online: boolean;
  targetId: number | null;
  lifecycleText: string;
  media: HomeDiskMedia;
}

export interface HomeDiskListSnapshot {
  disks: HomeDiskListItem[];
  autoMountCount: number;
}

export interface CreateMemoryDiskRequest {
  diskName: string;
  capacityMiB: number;
  requestedMemoryKind: MemoryCreateKind;
  autoMount: boolean;
}

export interface CreateMemoryDiskResponse {
  diskId: string;
}

export interface CreateFileDiskRequest {
  diskName: string;
  filePath: string;
  autoMount: boolean;
}

export interface CreateFileDiskResponse {
  diskId: string;
}

export interface CreateNewFileDiskRequest {
  diskName: string;
  filePath: string;
  capacityMiB: number;
  fileFormat: CreateFileFormat;
  autoMount: boolean;
}

export interface CreateNewFileDiskResponse {
  diskId: string;
}

export interface MountDiskRequest {
  diskId: string;
}

export interface MountDiskResponse {
  targetId: number;
}

export interface EjectDiskRequest {
  diskId: string;
}

export interface DeleteDiskRequest {
  diskId: string;
}

export interface DeleteDiskResponse {
  deletionId: string;
  undoAvailable: boolean;
}

export interface UndoDeleteDiskRequest {
  deletionId: string;
}

export interface CommitDeletedDiskRequest {
  deletionId: string;
}

export interface UpdateDiskRequest {
  diskId: string;
  diskName: string;
  autoMount: boolean;
}

export interface PickRawFilePathResponse {
  filePath: string | null;
}
