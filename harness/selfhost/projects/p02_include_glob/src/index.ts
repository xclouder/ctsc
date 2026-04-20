import { add, mul } from "./utils/math.js";
import { upper, repeat } from "./utils/string.js";
import { makeUser } from "./models/user.js";
import { deepEcho } from "./helpers/deep/nested.js";

export function demo(): string {
  const u = makeUser(1, "ada");
  const a = add(2, 3);
  const b = mul(a, 4);
  const s = upper(u.name);
  const r = repeat("-", 3);
  return deepEcho(s + r, String(b));
}
