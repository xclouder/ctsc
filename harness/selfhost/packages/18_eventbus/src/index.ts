import { EventBus } from "./bus.js";
import type { TickEvent, ErrorEvent } from "./types.js";

export function makeTickBus(): EventBus<TickEvent> {
  return new EventBus<TickEvent>();
}

export function makeErrorBus(): EventBus<ErrorEvent> {
  return new EventBus<ErrorEvent>();
}

export function run(frames: number): { ticks: number; errors: number } {
  const ticks = makeTickBus();
  const errors = makeErrorBus();
  let tickCount = 0;
  let errCount = 0;

  ticks.on((e) => { tickCount += e.frame; });
  errors.on((e) => { if (e.code === "E") errCount++; });

  for (let i = 1; i <= frames; i++) {
    ticks.emit({ frame: i, dt: 1 / 60 });
    if (i % 3 === 0) errors.emit({ code: "E", message: "x" });
  }

  return { ticks: tickCount, errors: errCount };
}
