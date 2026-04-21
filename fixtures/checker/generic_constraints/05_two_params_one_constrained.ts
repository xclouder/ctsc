// @checker: types
function pair<K extends string, V>(k: K, v: V): { k: K; v: V } {
  return { k, v };
}
const p = pair("name", 42);
