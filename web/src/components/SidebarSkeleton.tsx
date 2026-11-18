import { Fragment, useLayoutEffect, useRef, useState, type ReactNode } from 'react';
import { useStore } from '../state/store';
import { HeldText, Skeleton } from './Skeleton';
import { SettingsSkeleton } from './SettingsSkeleton';
import { railLayout, type RailLayout } from './rail-layout';
import { bootNotice, bootPagePrinter, bootShape, statusLineShown, type BootNote } from '../boot-shape';
import { noticeOfWire, ProjectNoticeRow } from './ProjectNotice';
import { printerTrigger } from '../profiles/metadata';
import { urlProject } from '../flow/routes';
import { materialNote } from '../printers/slots';
import './settings.css';
import './picker/picker.css';
import './menu.css';

const head = (title: string) => <header className="rail-head"><h2 className="rail-title"><span className="rail-fold"><span className="rail-title-text">{title}</span></span></h2></header>;
const printerPicker = (lines: { name: string; sub: string } | null) => lines
  ? <span className="pk-trigger dropdown" aria-hidden="true" style={{ flex: '1 1 0' }}><span className="pk-name">{lines.name}</span><span className="pk-sub">{lines.sub}</span></span>
  : <span className="pk-trigger dropdown skel-block" aria-hidden="true" style={{ flex: '1 1 0' }}><HeldText className="pk-name" text="Printer"/><HeldText className="pk-sub" text="0.4 mm"/></span>;
const printerRow = (live: boolean, lines: { name: string; sub: string } | null) => <div className="rail-row rail-printer"><div className="rail-lead"><Skeleton block className="rail-cover"/>{live && <span className="printer-live"><span className="printer-live-text status-text"><Skeleton width={40} style={{ display: 'inline-block' }}/></span></span>}</div><div className="rail-printer-controls">{printerPicker(lines)}</div></div>;
const line = (width: string) => <span className="skel-line" style={{ display: 'inline-block', width }}>{'\u00a0'}</span>;
const slotRow = (k: number, report: boolean, note: BootNote | undefined) => <div key={k} className="rail-row rail-slot"><Skeleton block width={20} height={20} style={{ justifySelf: 'center' }}/><Skeleton block height={32}/>
  <span className="rail-tail"><span className="icon-btn rail-act" aria-hidden="true"/></span>
  {report && <span className="rail-slot-report">{line('60%')}{note && <HeldText className="rail-slot-mismatch" text={materialNote(note.tray, note.name, note.type) ?? ''}/>}</span>}</div>;

function Remembered({ layout, printer, notice }: { layout: RailLayout; printer: ReactNode; notice: ReactNode }) {
  return <>{layout.sections.map((section, i) => (
    <section key={i} className="rail-section" style={{ height: section.height, boxSizing: 'border-box', position: 'relative', flex: 'none' }}>
      {section.rows.map((r, k) => <div key={k} style={{ position: 'absolute', left: 0, right: 0, top: r.top, height: r.height }}>
        {r.head !== undefined ? head(r.head) : r.group === 'printer' && printer ? printer : r.group === 'slots' && notice ? <div className="rail-slots" style={{ marginTop: 0 }}>{notice}</div> : <div className="rail-row" style={{ height: '100%', boxSizing: 'border-box' }}><span/><Skeleton block height={Math.min(32, Math.max(8, r.height - 12))}/><span/></div>}
      </div>)}
    </section>))}</>;
}

export function SidebarSkeleton() {
  const aside = useRef<HTMLElement>(null), [width, setWidth] = useState<number | null>(null);
  const own = useStore((s) => s.printer), printer = bootPagePrinter() ?? own, slots = useStore((s) => s.records.printers.find((p) => p.id === printer)?.slots.length);
  const notice = bootNotice(urlProject());
  const live = statusLineShown(useStore((s) => s.printerConnections.enabled));
  const reported = useStore((s) => s.printerConnections.connections.some((c) => c.printer === printer && c.kind === 'bambu-lan'));
  const boot = bootShape(printer || undefined), bootSlots = boot && (slots === undefined || boot.slots.length === slots) ? boot.slots : undefined;
  const pickerLines = boot?.picker && bootSlots ? printerTrigger(boot.picker) : null;
  useLayoutEffect(() => {
    const el = aside.current;
    if (!el) return;
    setWidth(el.getBoundingClientRect().width);
    const sizes = new ResizeObserver(() => setWidth(el.getBoundingClientRect().width));
    sizes.observe(el);
    return () => sizes.disconnect();
  }, []);
  const hint = width === null ? null : railLayout(printer, width, slots);
  return <aside ref={aside} className="sidebar" role="status" aria-label="Loading settings" data-layout={hint ? 'remembered' : 'generic'}>
    {hint ? <Remembered layout={hint} printer={pickerLines && printerRow(live, pickerLines)} notice={notice && <ProjectNoticeRow notice={noticeOfWire(notice.offer)}/>}/> : <>
      <section className="rail-section">{head('Printer')}{printerRow(live, pickerLines)}<div className="rail-slots">{notice && <ProjectNoticeRow notice={noticeOfWire(notice.offer)}/>}{Array.from({ length: slots ?? bootSlots?.length ?? 1 }, (_, k) => <Fragment key={k}>{bootSlots?.[k]?.group && <HeldText className="rail-slot-group" text="Left nozzle"/>}{slotRow(k, reported || !!bootSlots?.[k]?.report, bootSlots?.[k]?.note)}</Fragment>)}</div></section>
      <section className="rail-section process-section">{head('Print settings')}<div className="rail-row rail-row-wide"><Skeleton block height={32}/></div><SettingsSkeleton/></section>
    </>}
  </aside>;
}
