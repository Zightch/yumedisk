export type AppSessionPhase = "initializing" | "ready" | "failed";

export interface AppSessionSnapshot {
  ready: boolean;
  stateText: string;
}

export interface AppSessionRuntimeEvent {
  phase: AppSessionPhase;
  statusText: string;
}
