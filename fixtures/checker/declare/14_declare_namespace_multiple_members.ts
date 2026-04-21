// @checker: types
declare namespace Math2 {
  const PI: number;
  function floor(x: number): number;
  function ceil(x: number): number;
}
const p = Math2.PI;
const f = Math2.floor(1.5);
const c = Math2.ceil(1.5);
