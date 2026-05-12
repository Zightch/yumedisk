export type MemoryMediaKind = "denseMem" | "sparseMem";
export type FileMediaKind = "rawFile";
export type MemoryCreateKind = "auto" | MemoryMediaKind;

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
  connected: boolean;
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
