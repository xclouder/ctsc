// @checker: types
declare module "greet" {
  const g: (name: string) => string;
  export default g;
}
import g from "greet";
const r = g("hi");
