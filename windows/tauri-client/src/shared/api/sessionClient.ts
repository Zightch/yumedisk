import { invoke } from "@tauri-apps/api/core";
import type { SessionSnapshot } from "../../entities/session/model";

export async function restoreClientState(): Promise<void> {
  return invoke<void>("restore_client_state");
}

export async function openSession(): Promise<SessionSnapshot> {
  return invoke<SessionSnapshot>("open_session");
}

export function getErrorMessage(error: unknown): string {
  if (typeof error === "string") {
    return error;
  }

  if (error && typeof error === "object") {
    const message = Reflect.get(error, "message");
    if (typeof message === "string" && message.length > 0) {
      return message;
    }

    const detail = Reflect.get(error, "detail");
    if (typeof detail === "string" && detail.length > 0) {
      return detail;
    }

    const code = Reflect.get(error, "code");
    if (typeof code === "string" && code.length > 0) {
      return code;
    }
  }

  return "未知错误";
}
