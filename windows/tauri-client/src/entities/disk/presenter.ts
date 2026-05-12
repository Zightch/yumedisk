import type { HomeDiskListItem } from "./model";

const CAPACITY_UNITS = ["B", "KB", "MB", "GB", "TB", "PB"] as const;

export function formatDiskKindText(disk: HomeDiskListItem): string {
  if (disk.media.kind === "memory") {
    return `内存盘 · ${disk.media.memoryKind === "denseMem" ? "稠密" : "稀疏"}`;
  }

  return "文件盘 · RAW";
}

export function formatDiskCapacityText(disk: HomeDiskListItem): string {
  return formatBytes(disk.media.capacityBytes);
}

export function formatDiskDetailText(disk: HomeDiskListItem): string | null {
  if (disk.media.kind !== "file") {
    return null;
  }

  return disk.media.filePath;
}

function formatBytes(value: number): string {
  if (value <= 0) {
    return "0 B";
  }

  let index = 0;
  let size = value;

  while (size >= 1024 && index < CAPACITY_UNITS.length - 1) {
    size /= 1024;
    index += 1;
  }

  const text = size >= 100 || index === 0 ? size.toFixed(0) : size.toFixed(1);
  return `${text} ${CAPACITY_UNITS[index]}`;
}
