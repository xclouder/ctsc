import { parseList } from "./parser.js";

export function count(input: string): number {
  const tree = parseList(input);
  return tree.children.length;
}

export function describe(input: string): string {
  const tree = parseList(input);
  const values = tree.children.map((c) => c.value).join(",");
  return `n=${tree.children.length} values=${values}`;
}
