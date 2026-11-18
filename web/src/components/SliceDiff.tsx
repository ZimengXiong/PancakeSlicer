import { warningKey, type EngineWarning } from '../engine-warnings';
import { warningText } from '../engine-warning-text';
import { setCompareWanted, useBaseline, useStandardData } from '../compare';
import { editedOf, resolvedOf, settingChanges } from '../compare-settings';
import type { PreviewData } from '../preview/data';
import { readRecord, type RecordSummary } from '../preview/explain-record';
import { spacedTime } from '../preview/format';
import { useStore } from '../state/store';
import './slice-diff.css';

type Config = Record<string, unknown>;
const SHOWN = 12;

const time = (s: number | null) => (s == null ? '–' : spacedTime(s));
const metres = (record: Config | undefined) => {
  const mm = Number((record?.statistics as Config | undefined)?.total_used_filament_mm);
  return Number.isFinite(mm) ? `${(mm / 1000).toFixed(2)} m` : null;
};
const grams = (s: RecordSummary | null) => (s?.grams != null ? `${s.grams.toFixed(1)} g` : '–');
const top = (s: RecordSummary | null) => !s ? '–' : !s.curved ? 'Flat' : s.topCurved + s.topFlat > 0 ? `Curved ${Math.round((100 * s.topCurved) / (s.topCurved + s.topFlat))}%` : 'Curved';

export function SliceDiffSummary({ record }: { record: Config }) {
  const baseline = useBaseline();
  if (!baseline) return null;
  const before = readRecord(baseline.status.record as Config), after = readRecord(record);
  const stat = (label: string, a: string, b: string) => <div className="lg-stat"><span className="lg-stat-v">{b}</span><span className="lg-stat-k">{label}</span><span className="lg-stat-k">{a === b ? 'Unchanged' : `was ${a}`}</span></div>;
  return <div className="lg-summary sd-summary">{stat('Print time', time(before?.timeS ?? null), time(after?.timeS ?? null))}{stat('Filament', grams(before), grams(after))}</div>;
}

export function DoneComparing() {
  return <button type="button" className="btn sd-done" onClick={() => setCompareWanted(null)}>Done comparing</button>;
}

export function SliceDiff({ data, record }: { data: PreviewData; record: Config }) {
  const baseline = useBaseline();
  const beforeData = useStandardData();
  const signature = useStore((s) => s.job.plateSignature);
  const warnings = useStore((s) => s.job.warnings);
  if (!baseline) return null;
  const before = readRecord(baseline.status.record as Config), after = readRecord(record);
  const figures: [string, string, string][] = [
    ['Filament length', metres(baseline.status.record as Config) ?? '–', metres(record) ?? '–'],
    ['Layers', beforeData?.done ? String(beforeData.layers.length) : '…', data.done ? String(data.layers.length) : '…'],
    ['Top', top(before), top(after)],
  ];
  const now = warnings ?? [], was = baseline.warnings;
  const keys = (ws: EngineWarning[]) => new Set(ws.map(warningKey));
  const wasKeys = keys(was), nowKeys = keys(now);
  const added = now.filter((w) => !wasKeys.has(warningKey(w)) && warningText(w)), gone = was.filter((w) => !nowKeys.has(warningKey(w)) && warningText(w));
  const settings = settingChanges(resolvedOf(baseline.signature), resolvedOf(signature), new Set([...editedOf(baseline.signature), ...editedOf(signature)]));
  return (
    <section className="lg-section slice-diff" aria-label="Before and after">
      <div className="lg-section-title">Before and after</div>
      <ul className="sd-rows">
        {figures.map(([label, a, b]) => <li key={label} data-same={a === b || undefined}><span>{label}</span><span>{a === b ? b : `${a} → ${b}`}</span></li>)}
      </ul>
      {added.map((w) => <p key={`+${warningKey(w)}`} className="sd-warning">New warning: {warningText(w)}</p>)}
      {gone.map((w) => <p key={`-${warningKey(w)}`} className="sd-note">Resolved warnings: {warningText(w)}</p>)}
      <div className="lg-sec">Settings changed</div>
      {settings.length ? <ul className="sd-rows">
        {settings.slice(0, SHOWN).map((c) => <li key={c.label}><span>{c.label}</span><span>{c.from} → {c.to}</span></li>)}
        {settings.length > SHOWN && <li className="sd-more">and {settings.length - SHOWN} more</li>}
      </ul> : <p className="sd-note">Only the models or layout changed (settings stayed the same).</p>}
    </section>
  );
}
