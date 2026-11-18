import { readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { brotliCompressSync, constants, gzipSync } from 'node:zlib';
import type { Plugin } from 'vite';

const TEXT = /\.(js|mjs|css|json|svg|txt|map|wasm)$/;
const walk = (dir: string): string[] => readdirSync(dir).flatMap((name) => {
  const path = join(dir, name);
  return statSync(path).isDirectory() ? walk(path) : [path];
});

export function buildCompression(): Plugin {
  let outDir = '';
  return {
    name: 'orca-build-compression',
    apply: 'build',
    configResolved(config) { outDir = config.build.outDir.startsWith('/') ? config.build.outDir : join(config.root, config.build.outDir); },
    closeBundle() {
      for (const file of walk(outDir)) {
        if (!TEXT.test(file) || file.endsWith('index.html')) continue;
        const data = readFileSync(file);
        if (data.length < 1024) continue;
        writeFileSync(`${file}.gz`, gzipSync(data, { level: 9 }));
        writeFileSync(`${file}.br`, brotliCompressSync(data, { params: { [constants.BROTLI_PARAM_QUALITY]: 11, [constants.BROTLI_PARAM_SIZE_HINT]: data.length } }));
      }
    },
  };
}
