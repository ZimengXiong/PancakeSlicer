import { loadRuntimeOptions, type RuntimeOptions } from '../option-inventory';
import type { OptionDef, TabDesc } from '../options';

export type SettingsIn = { id: number; type: 'settings'; optionsUrl: string; tabUrl: string; native: boolean };
export type SettingsOut =
  | { id: number; type: 'settings'; options: Record<string, OptionDef>; tooltips?: string | null; tab: TabDesc }
  | { id: number; type: 'error'; message: string };

const FORMAT = 7;
const runtimeCache = new Map<string, RuntimeOptions>();
const post = (m: SettingsOut) => (self as unknown as Worker).postMessage(m);

let definitions: Promise<RuntimeOptions> | null = null, definitionsEngine = '';
let definitionsSent = false;
async function asset(url: string) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`${r.status} ${r.statusText}: ${url}`);
  return r.json();
}

async function runtimeOptions(url:string,native:boolean) {
  const key=`options|${FORMAT}|${url}|${native}`;
  const loaded=await loadRuntimeOptions(url,native,runtimeCache.get(key));
  runtimeCache.delete(key);runtimeCache.set(key,loaded);
  while(runtimeCache.size>2)runtimeCache.delete(runtimeCache.keys().next().value!);
  return loaded;
}

function loadDefinitions(url: string, native: boolean): Promise<RuntimeOptions> {
  const key = String(native);
  if (definitions && definitionsEngine === key) return definitions;
  definitionsEngine = key; definitionsSent = false;
  const loading: Promise<RuntimeOptions> = runtimeOptions(url, native).catch((error) => { if (definitions === loading) definitions = null; throw error; });
  return definitions = loading;
}

self.onmessage = async (e: MessageEvent<SettingsIn>) => {
  const { id } = e.data;
  try {
    const [loaded, tab] = await Promise.all([loadDefinitions(e.data.optionsUrl, e.data.native), asset(e.data.tabUrl)]);
    post({ id, type: 'settings', ...(definitionsSent ? { options: {} } : { options: loaded.options, tooltips: loaded.tooltips }), tab });
    definitionsSent = true;
  } catch (err) {
    post({ id, type: 'error', message: err instanceof Error ? err.message : String(err) });
  }
};
