import { useStore } from '../state/store';
import { SEP } from '../separator';
import { openDevice } from './sendOutcome';
import { statusLine } from './connections';
import { printerModel } from './capabilities';
import './printers.css';

export const shortName = (model: string) => {
  const words = model.replace(/^bambu\s*lab\s+/i, '').split(/\s+/).filter(Boolean);
  if (words.length > 1 && !/^bambu/i.test(model)) words.shift();
  let at = words.findIndex((w) => /\d/.test(w));
  if (at < 0) at = 0;
  const raw = (words[at] ?? '?').replace(/[^\w.]/g, '');
  const core = !/\d/.test(raw) ? raw.slice(0, 2).toUpperCase() : raw.length > 4 ? raw[0].toUpperCase() + raw.replace(/^\D+/, '') : raw.toUpperCase();
  const suffix = words.slice(at + 1).map((w) => w.replace(/\W/g, '')[0] ?? '').join('');
  return ((core.length + suffix.length > 4 ? core.replace(/^([A-Z])[A-Z]+(?=\d)/, '$1') : core) + suffix).slice(0, 4);
};

const R = 12, C = 2 * Math.PI * R;
interface Job { id: string; short: string; mark: string; detail: string; paused: boolean; progress: number }

export function PrintingChip() {
  const jobs = useStore((s) => {
    const list = s.printerConnections.connections.flatMap((c) => {
      const st = s.printerConnections.status[c.printer];
      if (!st?.online || (st.state !== 'printing' && st.state !== 'paused')) return [];
      const model = c.kind.startsWith('bambu') && c.model && c.model !== '?' ? `Bambu Lab ${c.model}` : printerModel(s, c.printer);
      const named = c.name && c.name !== model && c.name !== c.model ? c.name : '';
      return [{ id: c.id, short: shortName(model || c.name), mark: '', name: named, detail: `${named ? `${named}${SEP}` : ''}${model || c.name}: ${statusLine(st)}`,
        paused: st.state === 'paused', progress: st.progress ?? 0 }];
    });
    for (const j of list) {
      const same = list.filter((o) => o.short === j.short);
      if (same.length < 2) continue;
      const initials = same.map((o) => o.name[0]?.toUpperCase() ?? '');
      same.forEach((o, i) => { o.mark = initials.every((x, k) => x && initials.indexOf(x) === k) ? initials[i] : String(i + 1); });
    }
    return JSON.stringify(list.map(({ name: _, ...j }) => j));
  });
  const list = JSON.parse(jobs) as Job[];
  if (!list.length) return null;
  return <span className="print-rings">{list.map((j) =>
    <button key={j.id} type="button" className="print-ring" data-paused={j.paused || undefined} onClick={() => openDevice()}
      aria-label={`${j.detail}. Open Device`} data-truncated="true" data-full-text={j.detail}>
      <svg viewBox="0 0 28 28" aria-hidden="true">
        <circle className="print-ring-track" cx="14" cy="14" r={R} />
        <circle className="print-ring-done" cx="14" cy="14" r={R} strokeDasharray={C} strokeDashoffset={C * (1 - Math.min(1, Math.max(0, j.progress)))} />
      </svg>
      <span className="print-ring-name" data-long={j.short.length > 2 || undefined} data-longer={j.short.length > 3 || undefined}>{j.short}</span>
      {j.mark && <span className="print-ring-mark" aria-hidden="true">{j.mark}</span>}
    </button>)}</span>;
}
