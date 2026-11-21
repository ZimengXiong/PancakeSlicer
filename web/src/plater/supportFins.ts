import { Matrix4 } from 'three';
import { responseError } from '../api/access';
import { attachFiles, postJob } from '../api/blobs';
import { effectiveLayerHeight, slotNozzle } from '../profiles/context';
import { objectValues } from '../profiles/view';
import { jobValue } from '../options';
import { appendLibrary, plateSignature } from '../slice-input';
import { beginBusy } from '../state/busy';
import { undoable } from '../state/history';
import { effectiveConfig, getState, newId, setState, toast, type PlateObject, type State } from '../state/store';
import { bakeStl, loadMesh, objectMatrix } from './geometry';

const NOZZLES = [0.2, 0.25, 0.4, 0.6, 0.8, 1, 1.2, 1.4, 1.6, 1.8, 2];
const on = (v: unknown) => v === true || v === 1 || v === '1' || v === 'true';
const generated = (o: PlateObject) => (o.volumes ?? []).filter((v) => v.supportFins);

function nozzle(s: State, o: PlateObject): number {
  const index = slotNozzle(s, s.printer, Math.max(0, o.extruder - 1));
  return Number([effectiveConfig(s, 'machine').nozzle_diameter].flat()[index] ?? 0.4);
}

function nativeConflict(s: State, o: PlateObject): string | null {
  const values = { ...effectiveConfig(s, 'process'), ...objectValues(s, o.id).values, ...o.settings };
  if (on(values.enable_support)) return 'Turn off Enable support before using Support Fins, so native supports and fins are not both printed.';
  const mode = String(values.nonplanar_mode ?? 'off');
  if ((mode === 'auto' || mode === 'oneshot') && on(values.nonplanar_auto_support))
    return 'Turn off “Add support where a part would float” before using Support Fins, so native supports and fins are not both printed.';
  return null;
}

export function supportFinsEligibility(s: State, o: PlateObject | undefined): string | null {
  if (!o) return 'Select one model part first.';
  if (o.project || o.native) return 'Support Fins is available for editable model parts, not locked or assembled 3MF objects.';
  if ((o.volumes ?? []).some((v) => !v.supportFins))
    return 'Support Fins currently needs one solid model part without negative parts, modifiers, support volumes, or additional parts.';
  if (o.ranges?.length || o.layerProfile) return 'Support Fins currently needs one fixed layer height, not variable layer heights.';
  if (!NOZZLES.includes(nozzle(s, o))) return 'Support Fins currently supports 0.2, 0.25, 0.4, 0.6, 0.8, 1.0, 1.2, 1.4, 1.6, 1.8 and 2.0 mm nozzles.';
  const height = effectiveLayerHeight(s);
  if (!(height >= 0.08 && height <= 0.6)) return 'Support Fins needs a layer height between 0.08 and 0.6 mm.';
  return null;
}

export function supportFinsConflicts(s: State): string | null {
  const lines = s.objects.flatMap((o) => {
    if (!generated(o).length) return [];
    const conflict = nativeConflict(s, o);
    return conflict ? [`${o.name}: ${conflict}`] : [];
  });
  return lines.length ? lines.join('\n') : null;
}

let busy = false;

export async function generateSupportFins(objectId: number) {
  const s = getState(), o = s.objects.find((ob) => ob.id === objectId);
  if (busy || !o || s.tab !== 'prepare') return;
  const incompatible = supportFinsEligibility(s, o);
  if (incompatible) return toast('operations_failed', { detail: incompatible });
  if (!s.api || s.api.mock) return toast('operations_failed', { detail: 'Support Fins requires a connection to the slicing service.' });
  const conflict = nativeConflict(s, o);
  if (conflict) return toast('operations_failed', { detail: conflict });
  busy = true;
  const overlay = beginBusy(generated(o).length ? 'Regenerating Support Fins' : 'Generating Support Fins');
  const signature = plateSignature(s), hadFins = generated(o).length > 0;
  let id: string | undefined;
  try {
    setState({ toast: null });
    const fd = new FormData();
    fd.append('operation', 'support-fins');
    fd.append('support_fins_nozzle', String(nozzle(s, o)));
    fd.append('support_fins_layer_height', String(effectiveLayerHeight(s)));
    await attachFiles(fd, [{ name: 'support-fins-input.stl', blob: await bakeStl(o) }]);
    appendLibrary(fd, s, Object.fromEntries(Object.entries(o.settings).map(([key, value]) => [key, jobValue(key, value)])));
    const response = await postJob(fd);
    if (!response.ok) throw await responseError(response);
    id = (await response.json() as { id: string }).id;
    const status = await new Promise<import('../api/types').JobStatus>((resolve, reject) => {
      const stop = s.api!.events(id!, {
        progress() {},
        done(status) { stop(); resolve(status); },
        error(message) { stop(); reject(new Error(message)); },
      });
    });
    if (status.record?.schema !== 'orca.support-fins.v1') throw new Error('Support Fins returned an unreadable result.');
    if (signature !== plateSignature(getState())) throw new Error('The plate changed while Support Fins was running. Please try again.');
    if (status.record.fins !== true) {
      if (hadFins) undoable('Remove stale Support Fins', () => setState((current) => ({
        objects: current.objects.map((ob) => ob.id === objectId ? { ...ob, volumes: (ob.volumes ?? []).filter((v) => !v.supportFins) } : ob),
      })));
      toast('support_fins_none', { name: o.name });
      return;
    }
    const download = await fetch(s.api.downloadUrl(id));
    if (!download.ok) throw await responseError(download);
    const file = new File([await download.blob()], 'support-fins.stl', { type: 'model/stl' });
    const geometry = await loadMesh(file, undefined, false);
    const back = new Matrix4().makeTranslation(...geometry.userData.sourceCentre as [number, number, number]);
    if (signature !== plateSignature(getState())) {
      geometry.dispose();
      throw new Error('The plate changed while Support Fins was running. Please try again.');
    }
    const volume = {
      id: newId(), name: 'Support Fins (generated)', type: 'part' as const, geometry,
      matrix: objectMatrix(o).invert().multiply(back).elements, extruder: 0, settings: {}, supportFins: true,
    };
    undoable(hadFins ? 'Regenerate Support Fins' : 'Generate Support Fins', () => setState((current) => ({
      objects: current.objects.map((ob) => ob.id === objectId
        ? { ...ob, volumes: [...(ob.volumes ?? []).filter((v) => !v.supportFins), volume] }
        : ob),
      selected: { object: objectId, instance: o.instances[0].id, allInstances: true },
    })));
    toast('support_fins_added', { name: o.name });
  } catch (error) {
    toast('operations_failed', { detail: error instanceof Error ? error.message : String(error) });
  } finally {
    if (id) await s.api.cancel(id).catch(() => undefined);
    busy = false;
    overlay.end();
  }
}
