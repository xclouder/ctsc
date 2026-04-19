export class Animal {
  constructor(public readonly name: string) {}

  describe(): string {
    return "animal:" + this.name;
  }
}

export class Dog extends Animal {
  constructor(name: string, public breed: string) {
    super(name);
  }

  describe(): string {
    return super.describe() + "/" + this.breed;
  }
}
