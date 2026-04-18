function headTail<T>(xs: T[]): [T | undefined, T[]] {
  const [head, ...tail] = xs;
  return [head, tail];
}

function swap(pair: [number, number]): [number, number] {
  const [a, b] = pair;
  return [b, a];
}

const [x, y, z] = [1, 2, 3];
