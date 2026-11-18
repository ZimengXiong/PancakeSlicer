import { openUrlImport } from '../url-import';
import { NoticeLog } from './NoticeLog';
import { More } from './kit';
import { preloadable } from '../preloadable';
import { MenuButton, type MenuEntry } from './Menu';
import { Glyph } from './Glyph';
import { Check } from './Check';
import { Range } from './Range';
import { slicingText } from '../api/progress';
import { ACCESS_FAILURES, accessNotice, accessSnapshot, forgetAccessToken, saveAccessToken, subscribeAccess } from '../api/access';
import { useMachine } from '../flow/fsm';
import { libraryWrites } from '../profiles/library-writes';
import { Suspense, useEffect, useLayoutEffect, useMemo, useRef, useState, useSyncExternalStore, type ReactElement } from 'react';
import { addInstance, arrangeAll, cancelSlice, fileUse, openFileDialog, removeInstance, toolbarOperation, outsideVolumeWarning, canBoolean, toggleBoolean, toggleSvg, toggleText } from '../actions';
import { setCompareWanted } from '../compare';
import { toggleNonplanarView, useNonplanarView, usePatchPreview } from '../patch-preview';
import { applyRecommended, curveFailures, pendingRecommendations, plateResults, sliceFlatInstead, tryIgnoreDuct, trySmoothest, type CurveFailure } from '../recommend';
import { useSmoothestShare } from '../smoothest-alternative';
import { effectiveConfig, getState, mapPaint, printerPreset, setState, useStore, type Gizmo } from '../state/store';
import { IconButton } from './Icon';
import { Caret } from './Caret';
import { canPerformCut, cutBlocked, initialCut, setCut, toggleCut } from '../plater/cut';
import { closeSlantPlane, commitSlant, resetSlantAuto, setSlant } from '../plater/slant';
import { InTopLayer } from './Modal';
import { useTier } from '../breakpoints';
import { SliceFailed } from './Notices';
import { bareName } from '../engine-warnings';
import { plainWarnings, type PlainWarning } from '../engine-warning-text';

const OrientConfirm = preloadable(() => import('./OrientConfirm').then((m) => ({ default: m.OrientConfirm })));
const RemapFilaments = preloadable(() => import('./RemapFilaments'));
import { addPlate, canAddPlate } from '../plater/plates';
import { arrangeSettings, resetArrangeSettings, setArrangeSettings, subscribeArrangeSettings } from '../plater/arrangeSettings';
import { ANGLE, EXTRUDERS_LIMIT, GAP_AREA, HEIGHT, RADIUS, canPaint, gapFill, isPaint, paintOf, PAINT_STEP, PAINT_TOOLS, paintTool, setPaintTool, subscribePaintTool, togglePaint, type PaintKind } from '../plater/paint';
import { filamentColor } from '../plater/filamentColor';
import { canEditLayers } from './LayerHeight';
import { Num } from './Num';
import { setAssembly, useAssembly } from '../plater/assembly';
import { Sep } from '../separator';
import { slotFilaments } from '../profiles/slots';
import { PagePortal, usePageLeave } from './PageLayer';

const PAINT_GIZMOS: [PaintKind, string, string, string][] = [
  ['support', 'toolbar_support', 'Support painting', 'L'],
  ['seam', 'toolbar_seam', 'Seam painting', 'P'],
  ['fuzzy', 'toolbar_fuzzy_skin_paint', 'Fuzzy skin painting', 'H'],
  ['color', 'mmu_segmentation', 'Color painting', 'N'],
];

const GLYPH_BOX: Record<string, [number, number, number, number, number]> = {
  instance_add: [40, 4.5, 4.5, 31, 31], instance_remove: [40, 4.5, 4.5, 31, 31], menu_obj_svg: [16, 0, 0, 15, 15],
  toolbar_nonplanar_paint: [40, 4.5, 2.5, 32, 29.25],
};
export function ToolGlyph({ name, size }: { name: string; size: number }) {
  const [vb, x, y, w, h] = GLYPH_BOX[name] ?? [40, 4, 4, 31, 31], side = Math.max(w, h);
  const at = (start: number) => (vb === side ? 0 : (100 * start) / (vb - side));
  return <span className="tool-glyph" aria-hidden style={{ width: size, height: size, ['--glyph' as string]: `url("/images/${name}.svg")`,
    ['--glyph-size' as string]: `${(100 * vb) / side}%`, ['--glyph-at' as string]: `${at(x + w / 2 - side / 2)}% ${at(y + h / 2 - side / 2)}%` }} />;
}
const menuGlyph = (name: string) => <ToolGlyph name={name} size={16} />;

const MOD = /Mac|iPhone|iPad/.test(navigator.platform) ? 'Cmd' : 'Ctrl';

export function CanvasToolbar() {
  const gizmo = useStore((s) => s.gizmo);
  const hasObjects = useStore((s) => s.objects.length > 0);
  const meshPlates = useStore(canAddPlate);
  const sel = useStore((s) => s.selected);
  const instances = useStore((s) => s.objects.find((o) => o.id === s.selected?.object)?.instances.length ?? 0);
  const project = useStore((s) => !!s.project);
  const toggle = (g: Gizmo) => setState({ gizmo: gizmo === g ? 'none' : g });
  const booleanOk = useStore(canBoolean);
  const assemblyOn = useAssembly().on;
  const native = useStore((s) => !!s.objects.find((o) => o.id === s.selected?.object)?.native);
  const volumes = useStore((s) => !!s.objects.find((o) => o.id === s.selected?.object)?.volumes?.length);
  const locked = project ? ' — instances cannot be added or removed from an opened project' : '';
  const placeholder = useStore((s) => s.objects.find((o) => o.id === s.selected?.object)?.project?.placeholder);
  const edit = !!sel && !placeholder && !sel.all;
  const splitParts = useStore((s) => !s.objects.find((o) => o.id === s.selected?.object)?.volumes?.length);
  const cutBlock = useStore(cutBlocked);
  const paint = useStore(canPaint);
  const layers = useStore(canEditLayers);
  const preview = !!usePatchPreview();
  const nonplanarView = useNonplanarView();
  const [arranging, setArranging] = useState(false);
  usePageLeave(() => setArranging(false));
  const arrangeAt = useRef<HTMLSpanElement>(null);
  const rare: MenuEntry[] = [
    {label: 'Auto orient', shortcut: 'Q', icon: menuGlyph('toolbar_orient'), action: () => toolbarOperation('orient'), disabled: !hasObjects || project},
    {label: 'Add plate', icon: menuGlyph('toolbar_add_plate'), action: addPlate, disabled: !meshPlates},
    {label: 'Add instance', shortcut: '+', icon: menuGlyph('instance_add'), action: addInstance, disabled: !edit || project, tip: locked || undefined},
    {label: 'Remove instance', shortcut: '-', icon: menuGlyph('instance_remove'), action: removeInstance, disabled: instances < 2 || project, tip: locked || undefined},
    {label: 'Split to objects', icon: menuGlyph('split_objects'), action: () => toolbarOperation('split'), disabled: !edit || project},
    {label: 'Split to parts', icon: menuGlyph('split_parts'), action: () => toolbarOperation('split-parts'), disabled: !edit || project || !splitParts},
    'sep',
    {label: 'Lay on face', shortcut: 'F', icon: menuGlyph('toolbar_flatten'), action: () => toggle('flatten'), disabled: !edit, checked: gizmo === 'flatten'},
    {label: 'Cut', shortcut: 'C', icon: menuGlyph('toolbar_cut'), action: toggleCut, disabled: !!cutBlock && gizmo !== 'cut', tip: cutBlock ?? undefined},
    {label: 'Measure', shortcut: 'U', icon: menuGlyph('toolbar_measure'), action: () => toggle('measure'), disabled: !sel || !!placeholder, checked: gizmo === 'measure'},
    {label: 'Variable layer height', icon: menuGlyph('toolbar_variable_layer_height'), action: () => toggle('layers'), disabled: !layers && gizmo !== 'layers'},
    {label: 'Assembly view', icon: menuGlyph('toolbar_assembly'), action: () => setAssembly({on: !assemblyOn}), disabled: !hasObjects && !assemblyOn},
    {label: 'Mesh boolean', shortcut: 'B', icon: menuGlyph('toolbar_meshboolean'), action: toggleBoolean, disabled: !booleanOk && gizmo !== 'boolean'},
    'sep',
    ...PAINT_GIZMOS.map(([kind, icon, label, key]): MenuEntry => ({label, shortcut: key, icon: menuGlyph(icon), action: () => togglePaint(kind), disabled: !paint && gizmo !== kind, checked: gizmo === kind})),
    {label: 'Brim ears', icon: menuGlyph('toolbar_brimears'), action: () => toggle('brimears'), disabled: !sel || !!placeholder || !!native || volumes},
    {label: 'Emboss text', shortcut: 'T', icon: menuGlyph('toolbar_text'), action: toggleText, disabled: !!sel?.all || !!placeholder || !!native},
    {label: 'SVG', icon: menuGlyph('menu_obj_svg'), action: toggleSvg, disabled: !!sel?.all || !!placeholder || !!native},
    ...(preview ? [{label: 'Non-planar surfaces', icon: menuGlyph('toolbar_nonplanar_paint'), action: toggleNonplanarView, checked: nonplanarView} as MenuEntry] : []),
  ];
  const needSelection = !hasObjects ? 'add an object' : sel?.all ? 'select one object' : 'select an object';
  return <div className="canvas-toolbar">
    <ToolButton icon="toolbar_move" name="Move" keys="M" active={gizmo === 'move'} unavailable={!edit && needSelection} onClick={() => toggle('move')}/>
    <ToolButton icon="toolbar_rotate" name="Rotate" keys="R" active={gizmo === 'rotate'} unavailable={!edit && needSelection} onClick={() => toggle('rotate')}/>
    <ToolButton icon="toolbar_scale" name="Scale" keys="S" active={gizmo === 'scale'} unavailable={!edit && needSelection} onClick={() => toggle('scale')}/>
    <span className="tb-sep"/>
    <span ref={arrangeAt} style={{display: 'inline-flex'}}><ToolButton icon="toolbar_arrange" name="Arrange" keys="A" active={arranging} unavailable={!hasObjects && !arranging && 'add an object'} onClick={() => setArranging(!arranging)}/></span>
    {arranging && <ArrangeOptions at={arrangeAt.current}/>}
    <ToolButton icon="toolbar_orient" name="Auto orient" keys="Q" unavailable={!hasObjects ? 'add an object' : project && 'not in an opened 3MF project'} onClick={e => toolbarOperation('orient', e.shiftKey)}/>
    <MenuButton label="Add model" className="icon-btn tb-tool-btn" items={[{ label: 'Add model…', shortcut: `${MOD}+I`, action: openFileDialog }, { label: 'Import from URL…', action: () => openUrlImport() }]}><ToolGlyph name="toolbar_open" size={22} /></MenuButton>
    <MenuButton className="btn-ghost canvas-more" label="Model tools" items={rare}>Tools<Caret/></MenuButton>
  </div>;
}

function ToolButton({ icon, name, keys, active = false, unavailable, onClick }: { icon: string; name: string; keys: string; active?: boolean; unavailable?: string | false; onClick(e: React.MouseEvent<HTMLButtonElement>): void }) {
  return <button type="button" className={`icon-btn tb-tool-btn${active ? ' active' : ''}`} aria-label={`${name} (${keys})${unavailable ? `, ${unavailable}` : ''}`}
    aria-pressed={active} aria-disabled={unavailable ? true : undefined} onClick={(e) => { if (!unavailable) onClick(e); }}>
    <ToolGlyph name={icon} size={22} />
  </button>;
}

function ArrangeOptions({ at }: { at: HTMLElement | null }) {
  const a = useSyncExternalStore(subscribeArrangeSettings, arrangeSettings);
  const hasObjects = useStore((s) => s.objects.length > 0);
  const r = at?.getBoundingClientRect();
  return <PagePortal>
    <div className="manip-panel" role="dialog" aria-label="Arrange options" style={{ position: 'fixed', top: (r?.bottom ?? 40) + 8, left: r?.left ?? 0, zIndex: 5 }}>
      <div className="manip-grid" style={{ gridTemplateColumns: '70px 140px 70px' }}>
        <span>Spacing</span>
        <Range min={0} max={100} step={0.05} value={a.distance} onValueChange={(next) => setArrangeSettings({ distance: next })} />
        <Num value={a.distance} step={0.05} onCommit={(v) => setArrangeSettings({ distance: v })} />
        <span style={{ gridColumn: '1 / -1' }}>Set to 0 for automatic spacing.</span>
        <span style={{ gridColumn: '1 / -1' }} className="menu-sep" />
        <label className="select-row" style={{ gridColumn: '1 / -1' }}>
          <Check checked={a.rotation} onCheckedChange={(next) => setArrangeSettings({ rotation: next })} />Auto rotate for arrangement
        </label>
        {(
          <label className="select-row" style={{ gridColumn: '1 / -1' }}>
            <Check checked={a.multiMaterials} onCheckedChange={(next) => setArrangeSettings({ multiMaterials: next })} />Allow multiple materials on same plate
          </label>
        )}
        <span style={{ gridColumn: '1 / -1' }} className="menu-sep" />
      </div>
      <div style={{ display: 'flex', gap: 8, marginTop: 8 }}>
        <button className="btn primary" disabled={!hasObjects} onClick={() => arrangeAll()}>Arrange</button>
        <button className="btn" onClick={resetArrangeSettings}>Reset</button>
      </div>
    </div>
  </PagePortal>;
}

const cleanError = (msg = '') => msg.replace(/^(error:\s*)?(validate:\s*)?/i, '');

export function AccessNotification() {
  const notice = useSyncExternalStore(subscribeAccess, accessSnapshot);
  const [value, setValue] = useState('');
  const libraryOffline = useMachine(libraryWrites).value === 'offline';
  if (!notice || libraryOffline && [502, 503, 504].includes(notice.status)) return null;
  return (
    <div className={`notification ${notice.status === 429 || notice.status === 503 ? 'warning' : 'failed'}`} role="alert">
      <button className="notif-close icon-btn" aria-label="Close" onClick={() => accessNotice(null)}><Glyph name="close" size={14} /></button>
      <div className="notif-title">{notice.message}</div>
      {notice.status === 401 && (
        <form className="access-token-form" onSubmit={(event) => {
          event.preventDefault();
          setValue('');
          saveAccessToken(value).then(() => accessNotice(null), (error: Error) => accessNotice({ status: 401, message: error.message }));
        }}>
          <input type="password" aria-label="Access token" autoComplete="off" value={value} onChange={(event) => setValue(event.target.value)} />
          <button type="submit">Save</button>
          <button type="button" onClick={() => forgetAccessToken().then(() => accessNotice({ status: 401, message: 'Access token forgotten' }), (error: Error) => accessNotice({ status: 401, message: error.message }))}>Forget</button>
          <div className="notif-body">Save your access token, then give that another try.</div>
        </form>
      )}
      {notice.status === 507 && <div className="notif-body">Free up some disk space on the server (or ask your admin), then try again.</div>}
    </div>
  );
}

function TrySmoothest({ f }: { f: CurveFailure }) {
  const share = useSmoothestShare(f.objectId);
  if (share == null) return null;
  return <button type="button" className="notif-link" onClick={() => trySmoothest(f)}>Try Curved tops (plan: {Math.max(1, Math.round(share * 100))}% curved)</button>;
}

function WarningLines({ warnings, open, onToggle }: { warnings: PlainWarning[]; open: boolean; onToggle(): void }) {
  const lines = [...new Set(warnings.flatMap((w) => (w.text ? [w.text] : [])))];
  const unknown = warnings.filter((w) => w.detail);
  return <>
    {lines.map((l) => <div key={l} className="notif-body">{l}</div>)}
    {unknown.length > 0 && <div className="notif-body notif-actions"><button type="button" className="notif-link" aria-expanded={open} onClick={onToggle}>Details</button></div>}
    {open && unknown.map((w) => <div key={`${w.code}|${w.detail}`} className="notif-body notif-detail">{w.detail}</div>)}
  </>;
}

const TRANSIENT_MS = 8000;

export function SliceNotification() {
  const notice = useSyncExternalStore(subscribeAccess, accessSnapshot);
  const plateIndex = useStore((s) => s.plateIndex);
  const job = useStore((s) => s.job);
  const outside = useStore(outsideVolumeWarning);
  const [closed, setClosed] = useState<Set<string>>(new Set());
  const [expired, setExpired] = useState<Set<string>>(new Set());
  const brief = job.state === 'canceled' ? `canceled` : null;
  const done = job.state === 'done' && !job.stale && !job.exported ? `done:${job.id}` : null;
  const machine = useStore(printerPreset);
  const exported = job.exported ? `exported:${job.id}:${job.exported}` : null;
  useEffect(() => {
    const timers = ([[brief, 3000]] as const)
      .filter(([k]) => k)
      .map(([k, ms]) => setTimeout(() => setExpired((e) => new Set(e).add(k!)), ms));
    return () => timers.forEach(clearTimeout);
  }, [brief]);
  useEffect(() => setClosed(new Set()), [job.id]);
  const stack = useRef<HTMLDivElement>(null);
  useLayoutEffect(() => {
    const el = stack.current;
    if (!el) return;
    const observer = new MutationObserver((records) => { if (records.some((r) => r.addedNodes.length)) el.scrollTop = el.scrollHeight; });
    observer.observe(el, { childList: true });
    return () => observer.disconnect();
  }, []);
  const busy = job.state === 'running' || job.state === 'submitting';
  const close = (k: string) => (
    <button className="notif-close icon-btn" aria-label="Close" onClick={() => setClosed((c) => new Set(c).add(k))}>
      <Glyph name="close" size={14} />
    </button>
  );
  const sliced = useStore(plateResults);
  const failures = useStore(curveFailures);
  const failedIds = new Set(failures.map((f) => f.objectId));
  const results = done ? sliced.filter((r) => !failedIds.has(r.objectId)) : [];
  const suggestIds = useStore((s) => pendingRecommendations(s).map((o) => o.id).join(','));
  const tab = useStore((s) => s.tab);
  const plateObjects = useStore((s) => s.objects);
  const orientPrompts = useStore((s) => s.orientPrompts);
  const orientConfirm = useStore((s) => s.orientConfirm !== null);
  const suggest = useMemo(() => plateObjects.filter((o) => suggestIds.split(',').includes(String(o.id))), [plateObjects, suggestIds]);
  const raw = job.state === 'done' && !job.stale ? job.warnings : undefined;
  const names = plateObjects.map((o) => o.name).join('\n');
  const plain = useMemo(() => plainWarnings(raw ?? [], names.split('\n')), [raw, names]);
  const [details, setDetails] = useState<Set<string>>(new Set());
  const toggleDetails = (k: string) => setDetails((d) => { const n = new Set(d); if (n.has(k)) n.delete(k); else n.add(k); return n; });
  const perObject = plateObjects.flatMap((o) => {
    const bare = bareName(o.name), key = `${done ?? job.id}:o${o.id}`;
    const r = results.find((x) => x.objectId === o.id), f = failures.find((x) => x.objectId === o.id);
    const ws = plain.filter((w) => w.object === bare && (w.text !== null || w.detail));
    return r || f || ws.length ? [{ o, bare, key, r, f, ws }] : [];
  });
  const known = new Set(plateObjects.map((o) => bareName(o.name)));
  const unplaced = plain.filter((w) => (!w.object || !known.has(w.object)) && (w.text !== null || w.detail));
  const notes = unplaced.filter((w) => w.code.endsWith('_capped')), loose = unplaced.filter((w) => !notes.includes(w));
  const noteKey = notes.length ? `${job.id}:n` : null;
  const [held, setHeld] = useState(false);
  const transient = [exported, noteKey, ...perObject.filter((n) => n.r && !n.f && !n.ws.length && !n.r.slant?.why && !n.r.printedFlat).map((n) => n.key),
    ...Object.keys(orientPrompts).map((id) => `orient${id}`)].filter((k): k is string => !!k && !expired.has(k));
  useEffect(() => {
    if (held) return;
    const timers = transient.map((k) => setTimeout(() => setExpired((e) => new Set(e).add(k)), TRANSIENT_MS));
    return () => timers.forEach(clearTimeout);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [transient.join('|'), held]);
  const inPreview = tab === 'preview';
  const compareLink = <button type="button" className="notif-link" onClick={() => (setCompareWanted('standard'), setState({ tab: 'preview' }))}>Compare</button>;
  const tier = useTier();
  const [tipsOpen, setTipsOpen] = useState(false);
  const tips = [
    ...Object.keys(orientPrompts).map((id) => {
      const o = plateObjects.find((x) => String(x.id) === id);
      return o && !expired.has(`orient${id}`) && (
        <div key={`orient${id}`} className="notification done">
          <button className="notif-close icon-btn" aria-label="Close" onClick={() => dismissOrientPrompt(o.id)}>
            <Glyph name="close" size={14} />
          </button>
          <div className="notif-title">{bareName(o.name)}</div>
          <div className="notif-body">Tip: Stand this part upright to use less support<Sep/><button type="button" className="notif-link" onClick={() => void import('../plater/orientPrompt').then((m) => m.confirmOrientPrompt(o.id))}>Apply</button></div>
        </div>
      );
    }),
    exported && !expired.has(exported) && !closed.has(exported) && (
      <div key="exported" className="notification done">
        {close(exported)}
        <div className="notif-title">Exported successfully</div>
        <div className="notif-body">{job.exported}{'\n'}{fileUse(machine)}</div>
      </div>
    ),
    noteKey && !expired.has(noteKey) && !closed.has(noteKey) && (
      <div key="notes" className="notification done">
        {close(noteKey)}
        <WarningLines warnings={notes} open={details.has(noteKey)} onToggle={() => toggleDetails(noteKey)} />
      </div>
    ),
    brief && !expired.has(brief) && (
      <div key="brief" className="notification done">
        <div className="notif-title">Slicing canceled</div>
      </div>
    ),
    ...perObject.filter((n) => !n.f && !closed.has(n.key) && !expired.has(n.key) && !(inPreview && !n.ws.length && !n.r?.partial)).map(({ bare, key, r, ws }) => (
      <div key={key} className={`notification ${ws.length && !r ? 'warning' : 'done'}`}>
        {close(key)}
        <div className="notif-title notif-sentence">{r ? `${bare}: ${r.style}` : bare}</div>
        {r && !inPreview && <div className="notif-body">{r.fact}{r.curved && <><Sep/>{compareLink}</>}</div>}
        {r?.partial && <div className="notif-body">{r.partial[0].toUpperCase() + r.partial.slice(1)}</div>}
        <WarningLines warnings={ws} open={details.has(key)} onToggle={() => toggleDetails(key)} />
      </div>
    )),
  ].filter((el): el is ReactElement => !!el);
  useEffect(() => { if (tips.length < 2) setTipsOpen(false); }, [tips.length]);
  return (
    <div className="notifications" ref={stack} onPointerEnter={() => setHeld(true)} onPointerLeave={() => setHeld(false)}>
      <InTopLayer>
        <div className="notif-group">
          <AccessNotification />
          {job.state === 'failed' && notice?.message !== cleanError(job.error) && !ACCESS_FAILURES.has(job.errorCode ?? '') && !closed.has('error') && (
            <SliceFailed error={cleanError(job.error)} code={job.errorCode ?? 'slice_failed'} onClose={() => setClosed((c) => new Set(c).add('error'))} />
          )}
        </div>
      </InTopLayer>
      {outside && (
        <div className="notification warning" role="alert">
          <div className="notif-title">Model extends outside the build volume</div>
          <div className="notif-body">{outside}</div>
        </div>
      )}
      {suggest.length > 0 && !closed.has(`pip${suggestIds}`) && (
        <div className="notification warning">
          {close(`pip${suggestIds}`)}
          <div className="notif-title">{suggest.length === 1 ? bareName(suggest[0].name) : `${suggest.length} objects`}: Standard suggested</div>
          <div className="notif-body">Print-in-place gaps<Sep/><button type="button" className="notif-link" onClick={() => applyRecommended(suggest)}>Use Standard</button></div>
        </div>
      )}
      {orientConfirm && <Suspense fallback={null}><OrientConfirm /></Suspense>}
      {perObject.flatMap(({ f, key, ws }) => f ? [(
        <div key={`cnc${key}`} className="notification curve-failed" role="alert">
          <div className="notif-title notif-sentence">Oops! Couldn't slice {f.label} with {f.method}</div>
          <div className="notif-body">{f.why[0].toUpperCase() + f.why.slice(1)}</div>
          <WarningLines warnings={ws.filter((w) => w.code !== 'conformal_flat_fallback')} open={details.has(key)} onToggle={() => toggleDetails(key)} />
          <div className="notif-body notif-actions">
            <button type="button" className="notif-link" onClick={() => sliceFlatInstead([f])}>Slice flat instead</button>
            {f.tryDuct && <button type="button" className="notif-link" onClick={() => tryIgnoreDuct()}>Try Ignore cooling duct</button>}
            {f.mode === 'smooth_morph' && f.kind === 'hits' && <TrySmoothest f={f} />}
          </div>
        </div>
      )] : [])}
      {loose.length > 0 && !closed.has(`${job.id}:w`) && (
        <div className="notification warning">
          {close(`${job.id}:w`)}
          <div className="notif-title">Warning</div>
          <WarningLines warnings={loose} open={details.has(`${job.id}:w`)} onToggle={() => toggleDetails(`${job.id}:w`)} />
        </div>
      )}
      {busy && (
        <div className="notification running">
          <div className="notif-text">
            {slicingText(job, plateIndex)}
            <button className="notif-cancel icon-btn" aria-label="Cancel slicing" onClick={cancelSlice}>
              <Glyph name="close" size={14} />
            </button>
          </div>
          <div className="progress-row">
            <div className="progress" role="progressbar" aria-label={`Slicing plate ${plateIndex}`} aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(job.pct)}>
              <div className="progress-fill" style={{ width: `${job.pct}%` }} />
            </div>
          </div>
        </div>
      )}
      {tips.length > 1 && tier === 'phone' && (
        <button type="button" className="notif-chip" aria-expanded={tipsOpen} onClick={() => setTipsOpen(!tipsOpen)}>{tips.length} tips</button>
      )}
      {(tips.length < 2 || tier !== 'phone' || tipsOpen) && tips}
      <NoticeLog />
    </div>
  );
}

export { Toast } from './Notices';

export function SlantPanel() {
  const e = useStore((s) => (s.gizmo === 'slant' ? s.slant : null));
  const strict = useStore((s) => String(effectiveConfig(s, 'process').nonplanar_collision_check ?? 'warn') === 'block');
  if (!e) return null;
  const over = e.limit !== undefined && e.tilt > e.limit + 1e-6;
  const deg = (v: number) => `${v.toFixed(1)}°`;
  return (
    <div className="manip-panel slant-panel">
      <div className="manip-title">Slant plane</div>
      <div className="slant-readout" aria-live="polite">{e.auto ? 'Auto: ' : ''}tilt {deg(e.tilt)} toward {e.dirAuto ? 'the best direction' : deg(e.dir)}</div>
      <div className="slant-grid">
        <span>Tilt</span><Num label="Tilt" value={e.tilt} onCommit={(tilt) => setSlant({ tilt })} /><span>°</span>
        <span>Direction</span><Num label="Direction" value={e.dir} onCommit={(dir) => (dir < 0 ? setSlant({ dirAuto: true }) : setSlant({ dir }))} /><span>°</span>
        <span>Start height</span><Num label="Start height" value={e.z} onCommit={(z) => setSlant({ z })} /><span>mm</span>
      </div>
      {e.limit !== undefined && (over
        ? <div className="slant-warn" role="alert">Steeper than the head reaches ({deg(e.limit)}) in this direction: {strict
          ? `prints at ${deg(e.limit)}`
          : 'prints as set, though the head might give the part a little nudge'}</div>
        : <div className="slant-note">The head reaches {deg(e.limit)} in this direction</div>)}
      <div style={{ display: 'flex', gap: 8, marginTop: 8 }}>
        <button className="btn" disabled={e.auto} onClick={resetSlantAuto}>Reset to auto</button>
        <span style={{ flex: 1 }} />
        <button className="btn" onClick={closeSlantPlane}>Cancel</button>
        <button className="btn primary" onClick={commitSlant}>Done</button>
      </div>
    </div>
  );
}

const CutJoints = preloadable(() => import('./CutJoints'));

export function CutPanel() {
  const cut = useStore((s) => (s.gizmo === 'cut' ? s.cut : null));
  const can = useStore(canPerformCut);
  const joints = useStore((s) => !s.objects.find((o) => o.instances.some((i) => i.id === s.selected?.instance))?.project);
  if (!cut) return null;
  if (cut.editing) return <div className="manip-panel"><Suspense fallback={null}><CutJoints /></Suspense></div>;
  const joined = cut.mode === 'dovetail' || !!cut.connectors?.length;
  const initial = initialCut(getState());
  const part = (label: string, colour: string, keep: boolean, onKeep: (v: boolean) => void, place: boolean, onPlace?: (v: boolean) => void) => (
    <>
      <span style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
        <span style={{ width: 10, height: 10, background: colour, flex: 'none' }} />{label}
      </span>
      <label className="select-row" style={{ whiteSpace: 'nowrap' }}><Check checked={keep} disabled={joined} onCheckedChange={(next) => onKeep(next)} />Keep</label>
      <label className="select-row" style={{ whiteSpace: 'nowrap' }}>
        <Check checked={keep && place} disabled={!keep || !onPlace} onCheckedChange={(next) => onPlace?.(next)} />Place on cut
      </label>
    </>
  );
  return (
    <div className="manip-panel">
      <div className="manip-title">Cut</div>
      <div className="manip-grid">
        <span>Cut position</span>
        <Num value={cut.z} onCommit={(z) => setCut({ z })} />
        <span>mm</span>
        <span />
      </div>
      {joints && <Suspense fallback={null}><CutJoints /></Suspense>}
      <More id="cut">
        <div className="manip-grid">
          <span>Rotation</span>
          <Num value={cut.rotX} onCommit={(rotX) => setCut({ rotX })} />
          <Num value={cut.rotY} onCommit={(rotY) => setCut({ rotY })} />
          <span>°</span>
          <span style={{ gridColumn: '1 / -1' }}>After cut:</span>
        </div>
        <label className="select-row" style={{ whiteSpace: 'nowrap' }}>
          <Check checked={!!cut.toParts} disabled={joined} onCheckedChange={(next) => setCut({ toParts: next })} />Cut to parts
        </label>
        {!cut.toParts && (
          <div style={{ display: 'grid', gridTemplateColumns: 'auto auto auto', gap: '0 12px', alignItems: 'center' }}>
            {part('Upper part', '#00ffff', cut.keepUpper, (keepUpper) => setCut({ keepUpper }), true)}
            {part('Lower part', '#ff00ff', cut.keepLower, (keepLower) => setCut({ keepLower }), cut.placeLower, (placeLower) => setCut({ placeLower }))}
            {cut.keepUpper && <label className="select-row" style={{ gridColumn: '2 / span 2', whiteSpace: 'nowrap' }}>
              <Check checked={!!cut.flipUpper} onCheckedChange={(next) => setCut({ flipUpper: next })} />Flip upper part upside down
            </label>}
          </div>
        )}
      </More>
      <div style={{ display: 'flex', gap: 8, marginTop: 8 }}>
        <button className="btn" onClick={() => void import('../plater/cutJoints').then((m) => setCut(m.flipped(getState(), cut)))}>Flip</button>
        <button className="btn" disabled={!initial || (cut.z === initial.z && !cut.rotX && !cut.rotY && !cut.connectors?.length)}
          onClick={() => initial && setCut({ z: initial.z, rotX: 0, rotY: 0, connectors: [] })}>Reset</button>
        <span style={{ flex: 1 }} />
        <button className="btn primary" disabled={!can} onClick={() => void import('../plater/cutPerform').then((m) => m.performCut())}>Perform cut</button>
        <button className="btn" onClick={() => setState({ gizmo: 'none', cut: null })}>Cancel</button>
      </div>
    </div>
  );
}

const PAINT_TITLE: Record<PaintKind, string> = { support: 'Support painting', seam: 'Seam painting', fuzzy: 'Fuzzy skin painting', color: 'Color painting' };

export function PaintPanel() {
  const kind = useStore((s) => (isPaint(s.gizmo) ? s.gizmo : null));
  const object = useStore((s) => s.objects.find((o) => o.id === s.selected?.object));
  const t = useSyncExternalStore(subscribePaintTool, paintTool);
  const threshold = useStore((s) => object && kind === 'support' ? Number({ ...effectiveConfig(s, 'process'), ...object.settings }.support_threshold_angle) || 0 : 0);
  useEffect(() => { if (kind === 'support') setPaintTool({ highlight: threshold }); }, [kind, object?.id, threshold]);
  const filaments = useStore((s) => slotFilaments(s).map((_, i) => filamentColor(s, i + 1)).join(' ')).split(' ').filter(Boolean);
  useEffect(() => { if (kind && !PAINT_TOOLS[kind].some(([id]) => id === paintTool().tool)) setPaintTool({ tool: 'circle' }); }, [kind, t.tool]);
  useEffect(() => { if (t.filament > filaments.length) setPaintTool({ filament: 1 }); }, [t.filament, filaments.length]);
  if (!kind || !object) return null;
  const tools = PAINT_TOOLS[kind];
  const tool = tools.some(([id]) => id === t.tool) ? t.tool : 'circle';
  const fill = tool === 'fill', gap = tool === 'gap', height = tool === 'height', triangle = tool === 'triangle';
  const painted = !!paintOf(object, kind) || !!object.volumes?.some((v) => v.type === 'part' && paintOf(v, kind));
  return (
    <div className="manip-panel">
      <div className="manip-title">{PAINT_TITLE[kind]}</div>
      <div className="manip-grid" style={{ gridTemplateColumns: 'auto auto auto' }}>
        {kind === 'color' && (
          <>
            <span>Filaments</span>
            <span style={{ display: 'flex', gap: 4, flexWrap: 'wrap', gridColumn: 'span 2' }}>
              {filaments.slice(0, EXTRUDERS_LIMIT).map((c, i) => (
                <button key={i} type="button" className={`paint-filament ${t.filament === i + 1 ? 'active' : ''}`} style={{ background: c }}
                  aria-label={`Filament ${i + 1}`} aria-pressed={t.filament === i + 1} onClick={() => setPaintTool({ filament: i + 1 })}>{i + 1}</button>
              ))}
            </span>
          </>
        )}
        {kind === 'color' && filaments.length > 1 && <span style={{ gridColumn: '1 / -1' }}><Suspense fallback={null}><RemapFilaments object={object} colours={filaments} /></Suspense></span>}
        <span>Tool type</span>
        <span style={{ display: 'flex', gap: 2, gridColumn: 'span 2' }}>
          {tools.map(([id, icon, title]) => <IconButton key={id} icon={icon} title={title} size={20} active={tool === id} onClick={() => setPaintTool({ tool: id })} />)}
        </span>
        {gap ? (
          <>
            <span>Gap area</span>
            <Range min={GAP_AREA.min} max={GAP_AREA.max} step={GAP_AREA.step} value={t.gapArea} onValueChange={(next) => setPaintTool({ gapArea: next })} />
            <Num value={t.gapArea} step={GAP_AREA.step} onCommit={(v) => setPaintTool({ gapArea: v })} />
            <span />
            <button className="btn" style={{ gridColumn: 'span 2', justifySelf: 'start' }} disabled={!(t.gapArea > 0) || !painted}
              onClick={() => mapPaint(object.id, kind, PAINT_STEP[kind], (x, scale) => gapFill(x.geometry, paintOf(x, kind), t.gapArea / (scale * scale)))}>Apply</button>
          </>
        ) : triangle ? null : height ? (
          <>
            <span>Height range</span>
            <Range min={HEIGHT.min} max={HEIGHT.max} step={HEIGHT.step} value={t.height} onValueChange={(next) => setPaintTool({ height: next })} />
            <Num value={t.height} step={HEIGHT.step} onCommit={(v) => setPaintTool({ height: v })} />
          </>
        ) : fill && kind === 'color' && !t.edges ? null : (
          <>
            <span>{fill ? 'Smart fill angle' : 'Brush size'}</span>
            <Range min={fill ? ANGLE.min : RADIUS.min} max={fill ? ANGLE.max : RADIUS.max} step={fill ? ANGLE.step : RADIUS.step}
              value={fill ? t.angle : t.radius} onValueChange={(next) => setPaintTool(fill ? { angle: next } : { radius: next })} />
            <Num value={fill ? t.angle : t.radius} step={fill ? ANGLE.step : RADIUS.step} onCommit={(v) => setPaintTool(fill ? { angle: v } : { radius: v })} />
          </>
        )}
        {kind === 'color' && fill && (
          <label className="select-row" style={{ gridColumn: '1 / -1' }}>
            <Check checked={t.edges} onCheckedChange={(next) => setPaintTool({ edges: next })} />Edge detection
          </label>
        )}
        {kind === 'support' && !gap && (
          <label className="select-row" style={{ gridColumn: '1 / -1' }}>
            <Check checked={t.overhangsOnly} onCheckedChange={(next) => setPaintTool({ overhangsOnly: next })} />On highlighted overhangs only
          </label>
        )}
        {kind === 'support' && (
          <>
            <span style={{ gridColumn: '1 / -1' }} className="menu-sep" />
            <span>Highlight overhangs</span>
            <Range min={0} max={90} step={1} value={t.highlight} aria-label="Highlight overhangs" onValueChange={(next) => setPaintTool({ highlight: next })} />
            <Num value={t.highlight} step={1} onCommit={(v) => setPaintTool({ highlight: v })} />
          </>
        )}
      </div>
      <div style={{ display: 'flex', gap: 8, marginTop: 8 }}>
        <button className="btn" disabled={!painted} onClick={() => mapPaint(object.id, kind, PAINT_STEP[kind], () => null)}>Erase all</button>
        <button className="btn primary" onClick={() => setState({ gizmo: 'none' })}>Done</button>
      </div>
    </div>
  );
}

function dismissOrientPrompt(id: number) {
  setState((st) => {
    const orientPrompts = { ...st.orientPrompts };
    delete orientPrompts[id];
    return { orientPrompts, orientDismissed: [...st.orientDismissed, id] };
  });
}
