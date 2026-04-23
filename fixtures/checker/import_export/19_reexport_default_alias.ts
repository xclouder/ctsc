// @checker: types
declare module "greet" {
  export default function greet(name: string): string;
}
export { default as greeter } from "greet";
import { default as g } from "greet";
const x = g("ok");
