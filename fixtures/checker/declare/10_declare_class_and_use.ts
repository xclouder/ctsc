// @checker: types
declare class Counter {
  count: number;
  inc(): number;
}
declare const c: Counter;
const before = c.count;
const after = c.inc();
