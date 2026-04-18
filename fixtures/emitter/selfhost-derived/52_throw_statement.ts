function boom(msg: string): never {
  throw new Error("fatal: " + msg);
}

function guard(x: unknown): void {
  if (typeof x !== "string") {
    throw new TypeError("Expected a string");
  }
}
