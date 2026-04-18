interface Config {
  host: string;
  port: number;
}

function withDefaults(partial: Partial<Config>): Config {
  const defaults: Config = { host: "localhost", port: 80 };
  return { ...defaults, ...partial };
}

function merge(...objs: object[]): object {
  let out = {};
  for (const o of objs) {
    out = { ...out, ...o };
  }
  return out;
}
