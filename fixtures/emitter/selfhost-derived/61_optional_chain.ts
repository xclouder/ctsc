interface Nested {
  inner?: {
    value?: number;
    name?: string;
  };
}

function readValue(obj: Nested | null | undefined): number {
  return obj?.inner?.value ?? 0;
}

function readName(obj: Nested | null | undefined): string {
  return obj?.inner?.name ?? "unknown";
}

function firstChar(s: string | undefined | null): string {
  return s?.[0] ?? "";
}
