import { Rect, fromSize } from "./dist/index.js";
import assert from "node:assert/strict";

const r = new Rect({ x: 0, y: 0 }, { x: 3, y: 4 });
assert.equal(r.width(), 3);
assert.equal(r.height(), 4);
assert.equal(r.area(), 12);

const q = fromSize(10, 10, 5, 7);
assert.equal(q.area(), 35);
assert.equal(q.br.x, 15);
console.log("04_rect OK");
