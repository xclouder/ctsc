function raw(strings: TemplateStringsArray, ...values: unknown[]): string {
  let out = "";
  for (let i = 0; i < strings.length; i++) {
    out += strings[i];
    if (i < values.length) out += String(values[i]);
  }
  return out;
}

const greeting: string = raw`hello ${"world"}, answer=${42}`;
const plain: string = raw`no-subs-here`;

export function sql(strings: TemplateStringsArray, ...params: unknown[]): string {
  return strings.raw.join("?") + " :: " + params.join(",");
}

export const query: string = sql`SELECT ${1} FROM t WHERE x=${"a"}`;
