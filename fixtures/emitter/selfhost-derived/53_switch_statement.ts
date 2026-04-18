function unitToMs(unit: string, n: number): number {
  switch (unit) {
    case "s":
      return n * 1000;
    case "m":
      return n * 60_000;
    case "h":
      return n * 3_600_000;
    default:
      throw new Error("unknown unit: " + unit);
  }
}
