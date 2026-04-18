export function concat<T>(...arrs: T[][]): T[] {
  const out: T[] = [];
  for (const a of arrs) {
    out.push(...a);
  }
  return out;
}

export interface Config {
  host: string;
  port: number;
  tls: boolean;
}

export function withDefaults(partial: Partial<Config>): Config {
  const defaults: Config = { host: "localhost", port: 80, tls: false };
  return { ...defaults, ...partial };
}

export function headTail<T>(xs: T[]): [T | undefined, T[]] {
  const [head, ...tail] = xs;
  return [head, tail];
}
