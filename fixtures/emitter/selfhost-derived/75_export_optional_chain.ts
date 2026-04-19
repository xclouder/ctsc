export interface Addr {
  city?: string;
  zip?: number;
}

export interface User {
  name: string;
  addr?: Addr;
}

export function cityOf(u: User | null | undefined): string {
  return u?.addr?.city ?? "unknown";
}

export function zipOf(u: User | null | undefined): number {
  return u?.addr?.zip ?? -1;
}

export function firstLen(arr: string[] | undefined): number {
  return arr?.[0]?.length ?? 0;
}
