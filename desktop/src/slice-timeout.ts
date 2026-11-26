export const DEFAULT_SLICE_TIMEOUT_S = 900;
export const MIN_SLICE_TIMEOUT_S = 10;
export const MAX_SLICE_TIMEOUT_S = 1800;

export function isSliceTimeout(value: unknown): value is number {
  return typeof value === 'number' && Number.isInteger(value)
    && value >= MIN_SLICE_TIMEOUT_S && value <= MAX_SLICE_TIMEOUT_S;
}
