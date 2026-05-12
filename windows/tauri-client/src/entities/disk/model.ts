export type MemoryMediaKind = "denseMem" | "sparseMem";
export type FileMediaKind = "rawFile";
export type CreateFileFormat = "raw" | "vmdk" | "vhd" | "vhdx" | "vdi" | "qcow2";
export type MemoryCreateKind = "auto" | MemoryMediaKind;
export type DiskStatus = "disconnected" | "connected" | "invalid";

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
  autoConnect: boolean;
  readOnly: boolean;
  status: DiskStatus;
  invalidReason: string | null;
  online: boolean;
  targetId: number | null;
  lifecycleText: string;
  visiblePath: string;
  physicalDrivePath: string;
  media: HomeDiskMedia;
}

export interface HomeDiskListSnapshot {
  disks: HomeDiskListItem[];
  autoConnectCount: number;
}

export interface CreateMemoryDiskRequest {
  diskName: string;
  capacityMiB: number;
  requestedMemoryKind: MemoryCreateKind;
  autoConnect: boolean;
}

export interface CreateMemoryDiskResponse {
  diskId: string;
}

export interface CreateFileDiskRequest {
  diskName: string;
  filePath: string;
  autoConnect: boolean;
}

export interface CreateFileDiskResponse {
  diskId: string;
}

export interface CreateNewFileDiskRequest {
  diskName: string;
  filePath: string;
  capacityMiB: number;
  fileFormat: CreateFileFormat;
  autoConnect: boolean;
}

export interface CreateNewFileDiskResponse {
  diskId: string;
}

export interface ConnectDiskRequest {
  diskId: string;
}

export interface ConnectDiskResponse {
  targetId: number;
}

export interface DisconnectDiskRequest {
  diskId: string;
}

export interface DeleteDiskRequest {
  diskId: string;
}

export interface PickRawFilePathResponse {
  filePath: string | null;
}
