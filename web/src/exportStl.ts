import { zipSync } from 'fflate';
import { bakeStl, instanceMatrix } from './plater/geometry';
import { selectionTargets } from './edit';
import { getState, toast, type PlateObject } from './state/store';

const HEADER = 84, RECORD = 50;

const bakeInstances = (o: PlateObject) => Promise.all(o.instances.map((i) => bakeStl(o, instanceMatrix(o, i))));

async function oneStl(objects: PlateObject[]): Promise<Blob> {
  const parts = (await Promise.all(objects.map(bakeInstances))).flat();
  const tris = parts.reduce((n, b) => n + (b.size - HEADER) / RECORD, 0);
  const head = new ArrayBuffer(HEADER);
  new TextEncoder().encodeInto('PancakeSlicer web export', new Uint8Array(head, 0, 80));
  new DataView(head).setUint32(80, tris, true);
  return new Blob([head, ...parts.map((b) => b.slice(HEADER))], { type: 'model/stl' });
}

const stem = (name: string) => name.replace(/\.(stl|obj|3mf)$/i, '') || 'object';

function save(blob: Blob, name: string) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = name;
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 60000);
}

export async function exportStl(selection = false, multi = false) {
  const s = getState();
  const objects = (selection ? [...new Set(selectionTargets(s).map((t) => t.o))] : s.objects).filter((o) => !o.project?.placeholder);
  if (!objects.length) return;
  try {
    if (!multi) {
      const name = selection && objects.length === 1 ? objects[0].name : s.projectName ?? s.objects[0].name;
      return save(await oneStl(objects), `${stem(name)}.stl`);
    }
    const files: Record<string, Uint8Array> = {};
    for (const o of objects) {
      let name = `${stem(o.name)}.stl`;
      for (let n = 1; name in files; n++) name = `${stem(o.name)}(${n}).stl`;
      files[name] = new Uint8Array(await (await oneStl([o])).arrayBuffer());
    }
    save(new Blob([zipSync(files, { level: 0 })], { type: 'application/zip' }), `${stem(s.projectName ?? s.objects[0].name)}.zip`);
  } catch (e) {
    toast('export_failed', { detail: e instanceof Error ? e.message : e });
  }
}
