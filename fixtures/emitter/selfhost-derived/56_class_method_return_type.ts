class Rect {
  w: number = 0;
  h: number = 0;

  area(): number {
    return this.w * this.h;
  }

  scale(k: number): void {
    this.w *= k;
    this.h *= k;
  }
}
