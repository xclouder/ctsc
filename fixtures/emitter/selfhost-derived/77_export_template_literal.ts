export function greet(name: string, n: number): string {
  return `hello, ${name}! count=${n}`;
}

export function describe(title: string, body: string): string {
  return `=== ${title} ===
${body}
=== end ===`;
}

export const stamped: string = `[INFO] value=${42}`;
