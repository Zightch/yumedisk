import type { HomeDiskListItem } from "../../entities/disk/model";
import type { SessionPhase } from "../../entities/session/model";

export type HomeDiskDisplayPhase = "startup" | "normal";

export interface MapHomeDiskDisplayItemsOptions {
  sessionPhase: SessionPhase;
  diskDisplayPhase: HomeDiskDisplayPhase;
}

const STARTUP_INVALID_REASON = "正在初始化";
const SESSION_FAILED_INVALID_REASON = "会话失败";

export function mapHomeDiskDisplayItems(
  runtimeDisks: HomeDiskListItem[],
  options: MapHomeDiskDisplayItemsOptions,
): HomeDiskListItem[] {
  if (options.diskDisplayPhase === "normal") {
    return runtimeDisks.map((disk) => ({ ...disk }));
  }

  const invalidReason = options.sessionPhase === "failed"
    ? SESSION_FAILED_INVALID_REASON
    : STARTUP_INVALID_REASON;

  return runtimeDisks.map((disk) => ({
    ...disk,
    status: "invalid",
    invalidReason,
  }));
}
