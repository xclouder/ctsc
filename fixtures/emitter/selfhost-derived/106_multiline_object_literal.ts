// B19.e: object literals that are WRITTEN multi-line in the source must
// remain multi-line in the emitted output. tsc's printer honors source
// line positions; ctsc currently collapses them to a single line.
//
// Covers two self-host diffs:
//   - 07_enum-color    (computed keys, record-typed constant)
//   - 19_nested        (return { call(...), call(...), ... })

enum Color {
  Red,
  Green,
  Blue,
}

export const names: Record<Color, string> = {
  [Color.Red]: "red",
  [Color.Green]: "green",
  [Color.Blue]: "blue",
};

declare function sum(xs: number[]): number;
declare function max(xs: number[]): number;
declare function join(xs: string[], sep: string): string;

export function stats(values: number[], tags: string[]) {
  return {
    total: sum(values),
    peak: max(values),
    label: join(tags, ","),
  };
}
