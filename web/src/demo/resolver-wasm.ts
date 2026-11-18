import { wasm } from 'virtual:demo-resolver';

let compiled: Promise<WebAssembly.Module> | undefined;
export function compileResolver(): Promise<WebAssembly.Module> {
  return compiled ??= (async () => {
    const r = await fetch(wasm);
    if (!r.ok || !r.body) throw new Error(`Uh-oh, we couldn't download the demo resolver (${r.status}).`);
    return WebAssembly.compileStreaming(new Response(r.body.pipeThrough(new DecompressionStream('gzip')), { headers: { 'Content-Type': 'application/wasm' } }));
  })().catch((e) => { compiled = undefined; throw e; });
}
