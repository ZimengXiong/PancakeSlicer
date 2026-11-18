export function retainFlushSlots(matrix: (number | null)[] | null | undefined, count: number, kept: number[]): (number | null)[] | null {
  if (!matrix || count < 1 || matrix.length % (count * count)) return null;
  return Array.from({ length: matrix.length / (count * count) }, (_, nozzle) =>
    kept.flatMap((from) => kept.map((to) => matrix[nozzle * count * count + from * count + to]))).flat();
}

export function insertFlushSlots(matrix: (number | null)[], count: number, previous: number[]): (number | null)[] {
  const size = previous.length, area = count * count, nozzles = matrix.length / area;
  return Array.from({ length: nozzles * size * size }, (_, index) => {
    const from = previous[Math.floor(index / size) % size], to = previous[index % size];
    const nozzle = Math.floor(index / (size * size));
    return from < 0 || to < 0 ? null : matrix[nozzle * area + from * count + to];
  });
}
