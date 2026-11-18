import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { gzipSync } from 'node:zlib';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import type { Plugin } from 'vite';
import { catalogSlice, sampleSlice } from './demo-sample.ts';

let built: { wasm: Buffer; exec: Buffer } | undefined;
function resolver() {
  if (built) return built;
  const server = fileURLToPath(new URL('../server', import.meta.url)), out = join(mkdtempSync(join(tmpdir(), 'demo-resolve-')), 'resolve.wasm');
  execFileSync('go', ['build', '-trimpath', '-ldflags=-s -w', '-o', out, './cmd/demo-resolve'], { cwd: server, env: { ...process.env, GOOS: 'js', GOARCH: 'wasm' }, stdio: 'inherit' });
  const goroot = execFileSync('go', ['env', 'GOROOT'], { cwd: server }).toString().trim();
  return (built = { wasm: gzipSync(readFileSync(out), { level: 9 }), exec: readFileSync(join(goroot, 'lib/wasm/wasm_exec.js')) });
}

const BENCHY_KEY = 'sha256-3c44402d72219918df03d3f83bc93765fd0585ae0213a8d3724eec0ecdcfedd0';
const benchyPath = () => fileURLToPath(new URL(`./src/demo/fixtures/assets/${BENCHY_KEY}.gz`, import.meta.url));
async function benchyFixture() {
  const out = benchyPath();
  if (existsSync(out)) return;
  const decoderFile = fileURLToPath(new URL('./node_modules/three/examples/jsm/libs/draco/draco_decoder.js', import.meta.url));
  const module = { exports: {} as unknown };
  new Function('module', 'exports', 'require', '__filename', '__dirname', readFileSync(decoderFile, 'utf8'))(module, module.exports, createRequire(decoderFile), decoderFile, dirname(decoderFile));
  // eslint-disable-next-line @typescript-eslint/no-explicit-any -- Draco's emscripten module is untyped
  const draco: any = await (module.exports as () => Promise<unknown>)();
  const bytes = readFileSync(fileURLToPath(new URL('../engine/resources/handy_models/3DBenchy.drc', import.meta.url)));
  const buffer = new draco.DecoderBuffer(), decoder = new draco.Decoder(), mesh = new draco.Mesh();
  buffer.Init(new Int8Array(bytes.buffer, bytes.byteOffset, bytes.length), bytes.length);
  const status = decoder.DecodeBufferToMesh(buffer, mesh);
  if (!status.ok()) throw new Error(`3DBenchy.drc: ${status.error_msg()}`);
  const positions = new draco.DracoFloat32Array(), face = new draco.DracoInt32Array(), faces = mesh.num_faces();
  decoder.GetAttributeFloatForAllPoints(mesh, decoder.GetAttribute(mesh, decoder.GetAttributeId(mesh, draco.POSITION)), positions);
  const points = Float32Array.from({ length: positions.size() }, (_, i) => positions.GetValue(i));
  const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  points.forEach((v, i) => { lo[i % 3] = Math.min(lo[i % 3], v); hi[i % 3] = Math.max(hi[i % 3], v); });
  const centre = lo.map((v, c) => (v + hi[c]) / 2), stl = Buffer.alloc(84 + faces * 50);
  stl.write('3DBenchy', 0); stl.writeUInt32LE(faces, 80);
  for (let f = 0; f < faces; f++) {
    decoder.GetFaceFromMesh(mesh, f, face);
    for (let v = 0; v < 3; v++) for (let c = 0; c < 3; c++) stl.writeFloatLE(points[face.GetValue(v) * 3 + c] - centre[c], 84 + f * 50 + 12 + v * 12 + c * 4);
  }
  mkdirSync(fileURLToPath(new URL('./src/demo/fixtures/assets/', import.meta.url)), { recursive: true });
  writeFileSync(out, gzipSync(stl, { level: 9 }));
}

export function demoBuild(): Plugin {
  const id = '\0virtual:demo-resolver', routes = { wasm: '/@demo/resolve.wasm.gz', exec: '/@demo/wasm_exec.js' };
  return {
    name: 'pancake-static-demo',
    async buildStart() { catalogSlice(); await benchyFixture(); await sampleSlice(benchyPath()); },
    transformIndexHtml: {
      order: 'pre',
      handler: (html) => html.replace(/\s*<link rel="preload" href="\/api\/[^>]+>/g, '')
        .replace('/src/bootstrap.ts', '/src/demo/bootstrap.ts'),
    },
    transform(code, file) {
      if (/src\/(api\/settings|preview\/loader)\.worker\.ts$/.test(file))
        return `import { installDemoTransport } from '../demo/transport';\ninstallDemoTransport();\n${code}`;
    },
    resolveId(name) { if (name === 'virtual:demo-resolver') return id; },
    load(name) {
      if (name !== id) return;
      if (this.environment.mode === 'dev') return `export const wasm = ${JSON.stringify(routes.wasm)}, exec = ${JSON.stringify(routes.exec)};`;
      const { wasm, exec } = resolver();
      const w = this.emitFile({ type: 'asset', name: 'resolve.wasm.gz', source: wasm }), e = this.emitFile({ type: 'asset', name: 'wasm_exec.js', source: exec });
      return `export const wasm = import.meta.ROLLUP_FILE_URL_${w}, exec = import.meta.ROLLUP_FILE_URL_${e};`;
    },
    configureServer(server) {
      server.middlewares.use(routes.wasm, (_req, res) => { res.setHeader('Content-Type', 'application/gzip'); res.end(resolver().wasm); });
      server.middlewares.use(routes.exec, (_req, res) => { res.setHeader('Content-Type', 'text/javascript'); res.end(resolver().exec); });
    },
  };
}
