async function fetchNumber(): Promise<number> {
  return 42;
}

async function doubleIt(n: number): Promise<number> {
  const base = await fetchNumber();
  return base + n;
}

const handler = async (x: number): Promise<string> => {
  const v = await doubleIt(x);
  return "n=" + v;
};
