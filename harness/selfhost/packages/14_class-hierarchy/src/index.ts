export abstract class Shape {
  static count: number = 0;

  constructor() {
    Shape.count++;
  }

  abstract area(): number;

  describe(): string {
    return `Shape<area=${this.area()}>`;
  }
}

export class Circle extends Shape {
  constructor(public readonly r: number) {
    super();
  }

  area(): number {
    return Math.PI * this.r * this.r;
  }
}

export class Square extends Shape {
  constructor(public readonly side: number) {
    super();
  }

  area(): number {
    return this.side * this.side;
  }
}

export class Named<T extends Shape> {
  constructor(public readonly name: string, public readonly inner: T) {}

  label(): string {
    return this.name + ":" + this.inner.describe();
  }
}
