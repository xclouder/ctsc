export enum Color {
  Red,
  Green,
  Blue,
}

export const names: Record<Color, string> = {
  [Color.Red]: "red",
  [Color.Green]: "green",
  [Color.Blue]: "blue",
};

export function nameOf(c: Color): string {
  return names[c];
}
