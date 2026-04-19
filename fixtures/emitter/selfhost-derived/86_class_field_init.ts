export class Store {
  private counts = new Map<string, number>();
  private tags = new Set<string>();
  private created: number = Date.now();
  private readonly version: string = "v1";

  add(key: string, tag: string): void {
    this.counts.set(key, (this.counts.get(key) ?? 0) + 1);
    this.tags.add(tag);
  }

  info(): string {
    return this.version + "/" + this.counts.size + "/" + this.tags.size;
  }
}
