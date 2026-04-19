export function html(strings: TemplateStringsArray, ...values: unknown[]): string {
  let out = "";
  for (let i = 0; i < strings.length; i++) {
    out += strings[i];
    if (i < values.length) out += escape(String(values[i]));
  }
  return out;
}

function escape(s: string): string {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

export function sqlLike(strings: TemplateStringsArray, ...params: unknown[]): {
  text: string;
  params: unknown[];
} {
  const text = strings.reduce((acc, s, i) => acc + s + (i < params.length ? "?" : ""), "");
  return { text, params };
}
