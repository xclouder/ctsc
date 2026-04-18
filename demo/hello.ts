const greeting: string = "hello, ctsc";
const n: number = 42;

function add(a: number, b: number): number {
  return a + b;
}

interface Point { x: number; y: number; }
const p: Point = { x: 1, y: 2 };

console.log(greeting, n, add(p.x, p.y));
