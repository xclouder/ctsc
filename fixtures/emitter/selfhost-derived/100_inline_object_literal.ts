// B19.b: an object literal that fits on one line should NOT be split.
// ctsc always expands object literals that contain more than 1 key;
// tsc keeps short ones inline.
//
// Threshold is roughly `printLength <= 80` per the TS emitter rule.

export const small = { a: 1, b: 2, c: 3 };
export const rgb = { r: 255, g: 128, b: 0 };
export const point = { x: 1, y: 2 };

export function mkPair(): { k: string; v: number } {
  return { k: "key", v: 42 };
}
