// @checker: types
class Container<T extends { id: number }> {
  item: T;
  constructor(item: T) {
    this.item = item;
  }
  getId(): number {
    return this.item.id;
  }
}
declare const c: Container<{ id: number; name: string }>;
const x = c.item;
const y = c.getId();
