import { setState } from '../state/store';
import { fileAccept } from '../file-accept';

interface StreamLayers { layers: { first: number }[]; layerEnd: number[] }
let stream: { key: string; data: StreamLayers } | null = null;
export const setLoadedStream = (s: typeof stream) => { stream = s; };

export function layerOfMove(data: StreamLayers, move: number): number | null {
  let lo = 0, hi = data.layers.length - 1, k = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (data.layers[mid].first <= move) { k = mid; lo = mid + 1; } else hi = mid - 1;
  }
  return k >= 0 && move < data.layerEnd[k] ? k : null;
}

export const streamLayerOf = (jobId: string | undefined, move: number) =>
  stream?.key === `job:${jobId}` && stream.data.layers.length ? layerOfMove(stream.data, move) : null;

let pending: number | null = null;
let onJump: (() => void) | null = null;
export function jumpToMove(move: number) {
  pending = move;
  setState({ tab: 'preview' });
  onJump?.();
}
export const takeJump = () => { const m = pending; pending = null; return m; };
export const peekJump = () => pending;
export const setJumpListener = (l: typeof onJump) => { onJump = l; };

let pendingFile: File | null = null;
let onFile: ((f: File) => void) | null = null;
export function openGcode() {
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = fileAccept(['gcode', 'gco', 'g', '3mf']);
  input.onchange = () => {
    const f = input.files?.[0];
    if (!f) return;
    setState({ tab: 'preview' });
    if (onFile) onFile(f); else pendingFile = f;
  };
  input.click();
}
export const takeGcode = () => { const f = pendingFile; pendingFile = null; return f; };
export const setGcodeListener = (l: typeof onFile) => { onFile = l; };
