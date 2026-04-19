export abstract class Shape {
  static instances: number = 0;

  constructor() {
    Shape.instances++;
  }

  abstract area(): number;

  describe(): string {
    return "area=" + this.area();
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
