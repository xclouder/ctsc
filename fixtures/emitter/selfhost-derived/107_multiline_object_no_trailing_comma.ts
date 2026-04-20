// B19.f: companion to 106 - source is multi-line but the last property
// has NO trailing comma. tsc still keeps it multi-line; ctsc must too.

export const config = {
  host: "localhost",
  port: 8080,
  secure: false
};

export function make() {
  return {
    a: 1,
    b: 2,
    c: 3
  };
}
