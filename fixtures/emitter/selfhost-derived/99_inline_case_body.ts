// B19.a: `switch` case body is a single short statement.
// tsc's printer keeps `case X: return ...;` on one line when the body fits.
// ctsc currently always breaks after the colon and indents the body.
//
// Expected emit:
//   switch (n) {
//       case 1: return "one";
//       case 2: return "two";
//       case 3: return "three";
//       default: return "?";
//   }

export function spell(n: number): string {
  switch (n) {
    case 1: return "one";
    case 2: return "two";
    case 3: return "three";
    default: return "?";
  }
}
