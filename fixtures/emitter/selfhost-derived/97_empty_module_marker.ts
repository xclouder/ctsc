// Bug B20: a file whose only declarations are type-level (interface / type
// aliases / import type) becomes an EMPTY output once type erasure runs.
// tsc then injects `export {};` to keep the file a module (not a script)
// so tooling and `type="module"` resolution stay consistent.
// ctsc currently emits an empty string, which means Node treats the file
// as an (empty) CJS script.
//
// Expected output (tsc transpileModule, ES2020):
//   export {};
//
// verbatim. Nothing else.

export interface TickEvent {
  frame: number;
  dt: number;
}

export interface ErrorEvent {
  code: string;
  message: string;
}

export type Listener<T> = (payload: T) => void;
