import type { SlotOfferWire } from '../boot-shape';

export interface NoticeView { choice?: boolean; adds?: string; notes?: string[]; same?: boolean }
export const listed = (names: string[]) => names.length > 1 ? `${names.slice(0, -1).join(', ')} and ${names.at(-1)}` : names[0] ?? '';
export const noticeOfWire = (o: SlotOfferWire): NoticeView => ({ choice: o.differs || o.same, adds: o.differs || o.same ? listed(o.adds) : '', notes: o.notes, same: o.same });
export interface NoticeActions { use(): void; keep(): void; dismiss(): void }

export function ProjectNoticeRow({ notice, actions }: { notice: NoticeView; actions?: NoticeActions }) {
  const { choice = false, adds = '', notes = [] } = notice, held = !actions;
  return <div className="rail-row rail-project-slots" role="note" aria-busy={held || undefined}>
    {choice && <><span>{notice.same ? 'This project\'s filaments come with their own settings!' : 'This project used different filaments'}</span>
      <button type="button" className="btn" disabled={held} onClick={actions?.use}>{adds ? `Use project's (adds ${adds} to your library)` : 'Use project\'s'}</button>
      <button type="button" className="btn" disabled={held} onClick={actions?.keep}>Keep printer's</button></>}
    {notes.length > 0 && <p className="rail-project-note">{notes.join(' ')}{!choice && <button type="button" className="btn-ghost" disabled={held} onClick={actions?.dismiss}>Dismiss</button>}</p>}
  </div>;
}
