import { run, makeTickBus } from "./dist/index.js";
import assert from "node:assert/strict";

const r = run(6);
assert.equal(r.ticks, 1 + 2 + 3 + 4 + 5 + 6);
assert.equal(r.errors, 2);

const bus = makeTickBus();
let sum = 0;
const off1 = bus.on((e) => { sum += e.frame; });
bus.on((e) => { sum += e.frame * 10; });
assert.equal(bus.emit({ frame: 2, dt: 0 }), 2);
assert.equal(sum, 2 + 20);
assert.equal(bus.size, 2);
off1();
assert.equal(bus.size, 1);
assert.equal(bus.emit({ frame: 3, dt: 0 }), 1);
assert.equal(sum, 2 + 20 + 30);

console.log("18_eventbus: ok");
