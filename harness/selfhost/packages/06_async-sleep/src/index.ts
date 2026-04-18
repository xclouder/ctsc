export function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

export async function waitAndReturn<T>(value: T, ms: number): Promise<T> {
  await sleep(ms);
  return value;
}

export async function sumAfter(a: number, b: number, ms: number): Promise<number> {
  const delayed = await waitAndReturn(a + b, ms);
  return delayed;
}
