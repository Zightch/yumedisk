import type { SessionPhase } from "./model";

export function formatSessionPhaseText(sessionPhase: SessionPhase): string {
  if (sessionPhase === "ready") {
    return "会话正常";
  }

  if (sessionPhase === "failed") {
    return "会话失败";
  }

  return "正在初始化";
}

export function formatSessionPhaseDescription(
  sessionPhase: SessionPhase,
  sessionStatusText: string | null,
): string {
  if (sessionStatusText && sessionStatusText.trim().length > 0) {
    return sessionStatusText;
  }

  if (sessionPhase === "ready") {
    return "会话已打开，可以继续连接与管理磁盘。";
  }

  if (sessionPhase === "failed") {
    return "会话初始化失败，请重试。";
  }

  return "正在准备 Backend 会话与磁盘运行态。";
}
