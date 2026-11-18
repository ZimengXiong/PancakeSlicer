const tag = '$orcaBinary';
export { sha256 } from '../sha256';
const arrays = { Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array,
  Float32Array, Float64Array, BigInt64Array, BigUint64Array };
export function bytesBase64(bytes: Uint8Array): string {
  const chunks: string[] = [];
  for (let at = 0; at < bytes.length; at += 16384) chunks.push(String.fromCharCode(...bytes.subarray(at, at + 16384)));
  return btoa(chunks.join(''));
}
type BinaryField = Readonly<{ $orcaBinary: string; data: string }>;
function stringifyDocument(value: unknown, binaries?: WeakMap<object, BinaryField>): string {
  return JSON.stringify(value, function (this: Record<string, unknown>, key, item: unknown) {
    if (this[key] instanceof Date) return { $orcaDate: this[key].getTime() };
    if (item instanceof Map) return { $orcaMap: [...item.entries()] };
    if (item instanceof ArrayBuffer || ArrayBuffer.isView(item)) {
      const cached = binaries?.get(item);
      if (cached) return cached;
      const type = item instanceof ArrayBuffer ? 'ArrayBuffer' : item instanceof DataView ? 'DataView' : item.constructor.name;
      if (!(item instanceof ArrayBuffer) && type !== 'DataView' && !Object.hasOwn(arrays, type)) throw new Error(`Unsupported document array: ${type}`);
      const bytes = item instanceof ArrayBuffer ? new Uint8Array(item) : new Uint8Array(item.buffer, item.byteOffset, item.byteLength);
      const field = Object.freeze({ [tag]: type, data: bytesBase64(bytes) });
      binaries?.set(item, field);
      return field;
    }
    if (typeof item === 'number' && !Number.isFinite(item)) throw new Error('Project contains a non-finite number');
    return item;
  });
}
export function documentText(value: unknown): string { return stringifyDocument(value); }

export function prepareDocumentText(value: unknown) {
  const binaries = new WeakMap<object, BinaryField>();
  const stringify = (envelope: unknown) => stringifyDocument(envelope, binaries);
  return { text: stringify(value), stringify };
}
export function parseDocument<T>(text: string): T {
  return JSON.parse(text, (_key, item: unknown) => {
    if (item && typeof item === 'object' && Object.hasOwn(item, '$orcaDate') && Object.keys(item).length === 1) return new Date((item as {$orcaDate:number}).$orcaDate);
    if (item && typeof item === 'object' && Object.hasOwn(item, '$orcaMap') && Object.keys(item).length === 1 && Array.isArray((item as { $orcaMap: unknown }).$orcaMap)) return new Map((item as { $orcaMap: [unknown, unknown][] }).$orcaMap);
    if (!item || typeof item !== 'object' || !Object.hasOwn(item, tag)) return item;
    const { [tag]: type, data } = item as Record<string, unknown>;
    if (typeof type !== 'string' || typeof data !== 'string') throw new Error('Invalid binary document field');
    const bytes = Uint8Array.from(atob(data), (c) => c.charCodeAt(0));
    if (type === 'ArrayBuffer') return bytes.buffer;
    if (type === 'DataView') return new DataView(bytes.buffer);
    if (!Object.hasOwn(arrays, type)) throw new Error(`Unknown document array: ${type}`);
    const ArrayType = arrays[type as keyof typeof arrays];
    if (bytes.byteLength % ArrayType.BYTES_PER_ELEMENT) throw new Error('Truncated binary document field');
    return new ArrayType(bytes.buffer);
  }) as T;
}
