// @checker: types
declare module "entity" {
  export default class Entity {
    id: number;
  }
}
import type Entity from "entity";
declare const e: Entity;
const x = e;
