function identity<T>(x: T): T {
  return x;
}

function applyTwice<T extends () => void>(fn: T): void {
  fn();
  fn();
}

function pair<A, B>(a: A, b: B): [A, B] {
  return [a, b];
}
