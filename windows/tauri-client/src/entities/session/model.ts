export interface SessionSnapshot {
  ready: boolean;
  stateText: string;
}

export interface InitializeClientResponse {
  session: SessionSnapshot;
}
