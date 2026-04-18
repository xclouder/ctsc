export function greet(name: string, n: number): string {
  return `hello, ${name}! you are #${n}`;
}

export function block(title: string, body: string): string {
  return `== ${title} ==
${body}
== end ==`;
}

export function tag(strings: TemplateStringsArray, ...values: unknown[]): string {
  let out = "";
  for (let i = 0; i < strings.length; i++) {
    out += strings[i];
    if (i < values.length) out += `<${String(values[i])}>`;
  }
  return out;
}

export const stamped: string = tag`[${"INFO"}] x=${42}`;
