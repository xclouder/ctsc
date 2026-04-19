import { Shape, Circle, Square, Named } from "./dist/index.js";
import assert from "node:assert/strict";

Shape.count = 0;
const c = new Circle(2);
const s = new Square(3);
assert.ok(Math.abs(c.area() - Math.PI * 4) < 1e-9);
assert.equal(s.area(), 9);
assert.equal(Shape.count, 2);

const named = new Named("square3", s);
assert.equal(named.name, "square3");
assert.ok(named.label().startsWith("square3:"));
assert.ok(named.label().includes("area=9"));

console.log("14_class-hierarchy OK");
