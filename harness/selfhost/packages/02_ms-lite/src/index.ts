const S = 1000;
const M = S * 60;
const H = M * 60;
const D = H * 24;

export function ms(value: string): number {
  const match = /^(\d+)\s*(s|m|h|d)?$/.exec(value);
  if (!match) {
    throw new Error("Invalid input: " + value);
  }
  const n = parseInt(match[1], 10);
  const unit = match[2] || "s";
  switch (unit) {
    case "s": return n * S;
    case "m": return n * M;
    case "h": return n * H;
    case "d": return n * D;
    default:  throw new Error("Unknown unit: " + unit);
  }
}
