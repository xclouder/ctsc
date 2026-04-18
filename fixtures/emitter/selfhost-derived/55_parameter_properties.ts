class Point {
  constructor(public x: number, public y: number) {}

  translate(dx: number, dy: number): Point {
    return new Point(this.x + dx, this.y + dy);
  }
}
