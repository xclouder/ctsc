export interface Config {
  host: string;
  port: number;
  tls: boolean;
}

export function withDefaults(partial: Partial<Config>): Config {
  const defaults: Config = { host: "localhost", port: 80, tls: false };
  return { ...defaults, ...partial };
}

export function merge<T extends object, U extends object>(a: T, b: U): T & U {
  return { ...a, ...b };
}
