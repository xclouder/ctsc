// @checker: types
function pair<K extends string, V = K>(k: K, v: V): { k: K; v: V } {
  return { k, v };
}
const a = pair("name", 42);
const b = pair("id");
