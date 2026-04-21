// @checker: types
function f(tag: "a" | "b") {
  if (tag === "a") {
    const x = tag;
  }
}
