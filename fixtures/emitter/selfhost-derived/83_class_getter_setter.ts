export class Celsius {
  private _value: number = 0;

  get value(): number {
    return this._value;
  }

  set value(v: number) {
    this._value = v;
  }

  get fahrenheit(): number {
    return this._value * 9 / 5 + 32;
  }

  static fromFahrenheit(f: number): Celsius {
    const c = new Celsius();
    c.value = (f - 32) * 5 / 9;
    return c;
  }
}
