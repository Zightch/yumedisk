import type { AppSessionPhase } from "./model";

export function formatAppSessionPhaseText(appSessionPhase: AppSessionPhase): string {
  if (appSessionPhase === "ready") {
    return "会话正常";
  }

  if (appSessionPhase === "failed") {
    return "会话失败";
  }

  return "正在初始化";
}

export function formatAppSessionPhaseDescription(
  appSessionPhase: AppSessionPhase,
  appSessionStatusText: string | null,
): string {
  if (appSessionPhase === "ready") {
    return "会话已打开，可以继续挂载与管理磁盘。";
  }

  if (appSessionPhase === "failed") {
    if (appSessionStatusText && appSessionStatusText.trim().length > 0) {
      return appSessionStatusText;
    }

    return "会话初始化失败，请重试。";
  }

  if (appSessionStatusText && appSessionStatusText.trim().length > 0) {
    return appSessionStatusText;
  }

  return "正在准备 Backend 会话与磁盘运行态。";
}
