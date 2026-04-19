import { html, sqlLike } from "./dist/index.js";
import assert from "node:assert/strict";

assert.equal(html`<b>${"<x>"}</b>`, "<b>&lt;x&gt;</b>");
assert.equal(html`plain ${"&"} stuff`, "plain &amp; stuff");

const q = sqlLike`SELECT * FROM t WHERE id=${1} AND name=${"ada"}`;
assert.equal(q.text, "SELECT * FROM t WHERE id=? AND name=?");
assert.deepEqual(q.params, [1, "ada"]);

console.log("11_tagged-tpl OK");
