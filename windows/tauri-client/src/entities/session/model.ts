export type SessionPhase = "initializing" | "ready" | "failed";

export interface SessionSnapshot {
  ready: boolean;
  stateText: string;
}
