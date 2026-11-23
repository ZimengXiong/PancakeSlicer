import { build } from 'esbuild';

await build({
  entryPoints: ['src/mcp/index.ts'],
  outfile: 'out/mcp.cjs',
  bundle: true,
  platform: 'node',
  format: 'cjs',
  target: 'node20',
  legalComments: 'none',
  logLevel: 'warning',
});
