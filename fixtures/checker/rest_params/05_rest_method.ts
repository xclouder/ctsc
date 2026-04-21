// @checker: types
class Logger {
  log(prefix: string, ...args: string[]): void {}
}
declare const l: Logger;
l.log("info", "a", "b");
