const view = (data: ArrayBufferView | ArrayBuffer) => ArrayBuffer.isView(data) ? new Uint8Array(data.buffer, data.byteOffset, data.byteLength) : new Uint8Array(data);

export async function sha256(data: ArrayBufferView | ArrayBuffer | Blob): Promise<string> {
  const hasher = await (await import('hash-wasm')).createSHA256();
  if (data instanceof Blob) {
    const reader = data.stream().getReader();
    for (let r = await reader.read(); !r.done; r = await reader.read()) hasher.update(r.value);
  } else hasher.update(view(data));
  return hasher.digest('hex');
}
