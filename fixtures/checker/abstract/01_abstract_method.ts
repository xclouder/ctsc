// @checker: types
abstract class Shape {
  abstract area(): number;
}
class Square extends Shape {
  side: number = 1;
  area(): number {
    return this.side * this.side;
  }
}
const n = new Square().area();
