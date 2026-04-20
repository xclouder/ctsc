import { tokenize } from "./lexer.js";

export interface Tree {
  kind: "leaf" | "node";
  value: string;
  children: Tree[];
}

export function parseList(input: string): Tree {
  const tokens = tokenize(input);
  const root: Tree = { kind: "node", value: "root", children: [] };
  for (const t of tokens) {
    root.children.push({ kind: "leaf", value: t, children: [] });
  }
  return root;
}
