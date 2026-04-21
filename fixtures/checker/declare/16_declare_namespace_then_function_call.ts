// @checker: types
declare namespace IO {
  function read(): string;
  function write(s: string): void;
}
const line = IO.read();
IO.write(line);
