// @checker: types
class M {
  add(x: number): number;
  add(x: number, y: number): number;
  add(x: number, y?: number): number {
    return y === undefined ? x : x + y;
  }
}
declare const m: M;
const a = m.add(1);
const b = m.add(1, 2);
