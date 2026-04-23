// @checker: types
declare module "cjs" {
  const value: number;
  export = value;
}
import C = require("cjs");
const a = C;
