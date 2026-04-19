export class Counter {
  private n: number = 0;

  bump(by: number = 1): void {
    this.n += by;
  }

  value(): number {
    return this.n;
  }

  reset(to: number = 0, quiet: boolean = false): void {
    this.n = to;
    if (!quiet) {
      this.bump(0);
    }
  }
}

export function logLines(prefix: string = "info", ...lines: string[]): string {
  return lines.map((l) => "[" + prefix + "] " + l).join("\n");
}
