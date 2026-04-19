export interface Pos {
  x: number;
  y: number;
}

export class Vec2 {
  constructor(public x: number, public y: number) {}

  add(other: Vec2): Vec2 {
    return new Vec2(this.x + other.x, this.y + other.y);
  }

  magSq(): number {
    return this.x * this.x + this.y * this.y;
  }
}

export class Shape {
  constructor(public readonly name: string, private points: Vec2[]) {}
}
