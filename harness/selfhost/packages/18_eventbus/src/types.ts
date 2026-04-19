export interface TickEvent {
  frame: number;
  dt: number;
}

export interface ErrorEvent {
  code: string;
  message: string;
}
