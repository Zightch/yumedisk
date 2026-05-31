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
    }
  | {
      kind: "network";
      serverAddr: string;
      remoteDiskId: string;
      capacityBytes: number;
    };

export interface HomeDiskListItem {
  localDiskId: string;
  diskName: string;
  autoMount: boolean;
  configuredReadOnly: boolean;
  sourceReadOnly: boolean;
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

export interface RuntimeRescanState {
  running: boolean;
}

export interface RuntimeRescanStartResponse {
  started: boolean;
  running: boolean;
}

export type RuntimeRescanPhase = "started" | "finished" | "failed";

export interface RuntimeRescanLifecycleEvent {
  phase: RuntimeRescanPhase;
  errorText: string | null;
}

export interface CreateMemoryDiskRequest {
  diskName: string;
  capacityMiB: number;
  requestedMemoryKind: MemoryCreateKind;
  autoMount: boolean;
}

export interface CreateMemoryDiskResponse {
  localDiskId: string;
}

export interface CreateFileDiskRequest {
  diskName: string;
  filePath: string;
  autoMount: boolean;
  configuredReadOnly: boolean;
}

export interface CreateFileDiskResponse {
  localDiskId: string;
}

export interface CreateNewFileDiskRequest {
  diskName: string;
  filePath: string;
  capacityMiB: number;
  fileFormat: CreateFileFormat;
  autoMount: boolean;
}

export interface CreateNewFileDiskResponse {
  localDiskId: string;
}

export interface MountDiskRequest {
  localDiskId: string;
}

export interface MountDiskResponse {
  targetId: number;
}

export interface EjectDiskRequest {
  localDiskId: string;
}

export interface DeleteDiskRequest {
  localDiskId: string;
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
  localDiskId: string;
  diskName: string;
  autoMount: boolean;
  configuredReadOnly: boolean;
}

export interface PickRawFilePathResponse {
  filePath: string | null;
}

export interface TestNetworkConnectionRequest {
  serverAddr: string;
}

export interface CreateNetworkDraftRequest {
  serverAddr: string;
}

export interface NetworkDraftItem {
  diskName: string;
  serverAddr: string;
  remoteDiskId: string;
  capacityBytes: number;
  readOnly: boolean;
}

export interface NetworkDraftSnapshot {
  draftId: string;
  serverAddr: string;
  items: NetworkDraftItem[];
}

export interface AddNetworkDraftItemRequest {
  draftId: string;
  diskName: string;
  claimCode: string;
}

export interface RemoveNetworkDraftItemRequest {
  draftId: string;
  remoteDiskId: string;
}

export interface SubmitNetworkDraftRequest {
  draftId: string;
}

export interface DisposeNetworkDraftRequest {
  draftId: string;
}
