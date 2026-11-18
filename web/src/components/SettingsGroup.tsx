import { Glyph } from './Glyph';
import { memo, useSyncExternalStore, type ReactNode } from 'react';
import { SectionHeading } from './SectionHeading';

const expanded = new Set<string>(), folded = new Set<string>(), listeners = new Set<() => void>();
let version = 0;
const subscribe = (listener: () => void) => { listeners.add(listener); return () => { listeners.delete(listener); }; };

export function setGroupOpen(id: string, open: boolean) {
  if (open === expanded.has(id) && open !== folded.has(id)) return;
  if (open) { expanded.add(id); folded.delete(id); } else { expanded.delete(id); folded.add(id); }
  version++;
  listeners.forEach((listener) => listener());
}
export const isGroupOpen = (id: string, byDefault = false) => expanded.has(id) || (byDefault && !folded.has(id));
export const useSettingsGroups = () => useSyncExternalStore(subscribe, () => version);

export type Mark = 'unsaved' | undefined;
export const markOf = (keys: string[], unsaved: (key: string) => boolean): Mark => keys.some(unsaved) ? 'unsaved' : undefined;
export const GroupHead = memo(function GroupHead({ id, title, modified, open, aside, tag, onReset }: { id: string; title: string; modified: Mark; open: boolean; aside?: ReactNode; tag?: string;  onReset?: () => void }) {
  const content = <>
    <svg viewBox="0 0 12 12" width="12" height="12" aria-hidden><path d="M2.5 4.5 6 8l3.5-3.5" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" /></svg>
    <span>{title}</span>
    {tag && <span className="optgroup-tag">{tag}</span>}
    {modified && <span className="field-label-hidden">(changed)</span>}
  </>;
  const head = (
    <SectionHeading level="sub" className="optgroup-title">
      <button type="button" className={`optgroup-head ${modified ? `modified ${modified}` : ''}`} aria-expanded={open} onClick={() => setGroupOpen(id, !open)}>{content}</button>
    </SectionHeading>
  );
  const reset = modified === 'unsaved' && onReset && <button type="button" className="reset-icon section-reset" title="Reset section" aria-label={`Reset ${title}`} onClick={onReset}><Glyph name="undo" size={14}/></button>;
  return aside || reset ? <div className="optgroup-headrow">{head}{reset}{aside && <div className="optgroup-aside">{aside}</div>}</div> : head;
});
