export interface Point {
  x: number;
  y: number;
}

export class Rect {
  constructor(public tl: Point, public br: Point) {}

  width(): number {
    return this.br.x - this.tl.x;
  }

  height(): number {
    return this.br.y - this.tl.y;
  }

  area(): number {
    return this.width() * this.height();
  }
}

export function fromSize(x: number, y: number, w: number, h: number): Rect {
  return new Rect({ x, y }, { x: x + w, y: y + h });
}
