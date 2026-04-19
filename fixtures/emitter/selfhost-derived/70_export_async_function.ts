export async function fetchOne(): Promise<number> {
  return 1;
}

export async function fetchTwo(x: number): Promise<number> {
  const base = await fetchOne();
  return base + x;
}

export const fetchThree = async (): Promise<number> => {
  const v = await fetchTwo(2);
  return v;
};
