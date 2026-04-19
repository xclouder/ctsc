export type Listener<T> = (payload: T) => void;

export class EventBus<T> {
  private listeners: Listener<T>[] = [];

  on(fn: Listener<T>): () => void {
    this.listeners.push(fn);
    return () => {
      const i = this.listeners.indexOf(fn);
      if (i >= 0) this.listeners.splice(i, 1);
    };
  }

  emit(payload: T): number {
    let n = 0;
    for (const fn of this.listeners) {
      fn(payload);
      n++;
    }
    return n;
  }

  get size(): number {
    return this.listeners.length;
  }
}
