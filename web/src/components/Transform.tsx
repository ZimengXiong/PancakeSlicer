import { Suspense, useEffect, useState } from 'react';
import { preloadable } from '../preloadable';
import { Glyph } from './Glyph';
import { Popover } from 'radix-ui';
import * as THREE from 'three';
import { setState, updateInstance, updateObject, useStore, markStale } from '../state/store';
import { undoable } from '../state/history';
import { Num, type NumIssue } from './Num';
import { Dropdown } from './Dropdown';
import { arrangeAll, toolbarOperation } from '../actions';
import { canDrop, centreSelection, dropSelection } from '../edit';
import { InLayer } from './PageLayer';

const PartManipulation = preloadable(() => import('./PartManipulation'));

let moveCoordinates: 'world' | 'object' = 'world';
const DEG = Math.PI / 180;
const AX = ['X', 'Y', 'Z'] as const;
const aboveZero = (v: number): NumIssue | undefined => v > 0 ? undefined : { inline: 'Needs to be above 0!', detail: 'needs to be above 0!' };
const rotationOf = (rot: [number, number, number]) => new THREE.Matrix4().makeRotationZ(rot[2] * DEG)
  .multiply(new THREE.Matrix4().makeRotationY(rot[1] * DEG)).multiply(new THREE.Matrix4().makeRotationX(rot[0] * DEG));

function setRotation(objectId: number, instanceId: number, rot: [number, number, number]) {
  undoable('Reset rotation', () => setState((s) => ({
    objects: s.objects.map((o) => (o.id !== objectId ? o : {
      ...o, rotX: rot[0], rotY: rot[1], instances: o.instances.map((i) => (i.id === instanceId ? { ...i, rotZ: rot[2] } : i)),
    })),
    job: markStale(s.job),
  })));
}

export function CoordToggle({ value, onChange, enabled }: { value: 'world' | 'object'; onChange: (v: 'world' | 'object') => void; enabled: boolean }) {
  return (
    <Dropdown aria-label="Coordinates" value={value} disabled={!enabled} onValueChange={(next) => onChange(next as 'world' | 'object')}
      options={[{ value: 'world', label: 'World' }, { value: 'object', label: 'Object' }]} />
  );
}

function RowButton({ d, title, onClick, disabled, pressed }: { d: string; title: string; onClick: () => void; disabled?: boolean; pressed?: boolean }) {
  return (
    <button type="button" className="xf-btn" aria-label={title} aria-pressed={pressed} disabled={disabled} onClick={onClick}>
      <svg width="16" height="16" viewBox="0 0 16 16" aria-hidden="true"><path d={d} fill="none" stroke="currentColor" strokeWidth="1.4" strokeLinejoin="round" /></svg>
    </button>
  );
}

export function Row({ label, values, onCommit, names, action }: { label: string; values?: number[]; onCommit: (axis: number, v: number) => void; names: string; action?: React.ReactNode }) {
  return (
    <>
      <span className="xf-key">{label}</span>
      {[0, 1, 2].map((a) => <Num key={a} axis={AX[a]} label={`${names} ${AX[a]}`} value={values?.[a]} onCommit={(v) => onCommit(a, v)} />)}
      <span className="xf-act">{action}</span>
    </>
  );
}

export function TransformPanel() {
  const gizmo = useStore((s) => s.gizmo);
  const sel = useStore((s) => s.selected);
  const o = useStore((s) => (s.selected && !s.selected.all ? s.objects.find((ob) => ob.id === s.selected!.object) : undefined));
  const uniform = useStore((s) => s.uniformScale);
  const inst = o?.instances.find((i) => i.id === sel?.instance);
  const [coordinates, setCoordinates] = useState(moveCoordinates), [more, setMore] = useState(moveCoordinates === 'object');
  const project = useStore((s) => !!s.project);
  const active = gizmo === 'move' || gizmo === 'rotate' || gizmo === 'scale';
  const valid = !!o && !!inst && !sel?.all && !o.project?.placeholder;
  const close = () => setState({gizmo: 'none'});
  useEffect(() => { if (active && !valid) setState({gizmo: 'none'}); }, [active, valid]);
  const openKey = `${gizmo}|${o?.id}|${inst?.id}`;
  const [opened, setOpened] = useState<{ key: string; pos: THREE.Vector3; rot: [number, number, number] } | null>(null);
  if (o && inst && opened?.key !== openKey) setOpened({ key: openKey, pos: new THREE.Vector3(inst.x, inst.y, inst.z ?? 0), rot: [o.rotX, o.rotY, inst.rotZ] });
  const title = gizmo === 'move' ? 'Move' : gizmo === 'rotate' ? 'Rotate' : gizmo === 'scale' ? 'Scale' : 'Transform';
  const v = o?.volumes?.find((vv) => vv.id === sel?.volume);
  const live = o && inst && !o.project?.placeholder ? o : undefined;
  const lock = <RowButton pressed={uniform} title={uniform ? 'Keep proportions: on' : 'Keep proportions: off'} onClick={() => setState({ uniformScale: !uniform })}
    d={uniform ? 'M5 7V5a3 3 0 0 1 6 0v2M4 7h8v6H4z' : 'M5 7V5a3 3 0 0 1 6 0M4 7h8v6H4z'} />;
  let body: React.ReactNode = null;
  if (live && inst && v) body = <Suspense fallback={null}><PartManipulation o={live} inst={inst} v={v} local={coordinates === 'object' && gizmo === 'move'} lock={lock} /></Suspense>;
  else if (live && inst) {
    const o = live;
    const rot: [number, number, number] = [o.rotX, o.rotY, inst.rotZ];
    const start = opened?.key === openKey ? opened : { pos: new THREE.Vector3(inst.x, inst.y, inst.z ?? 0), rot };
    if (gizmo === 'move') {
      const local = coordinates === 'object';
      const axes = rotationOf(rot);
      const relative = new THREE.Vector3(inst.x, inst.y, inst.z ?? 0).sub(start.pos).applyMatrix4(axes.clone().transpose());
      const moveTo = (axis: number, value: number) => {
        if (!local) return updateInstance(o.id, inst.id, [{ x: value }, { y: value }, { z: value }][axis]);
        const p = relative.clone().setComponent(axis, value).applyMatrix4(axes).add(start.pos);
        updateInstance(o.id, inst.id, { x: p.x, y: p.y, z: p.z });
      };
      const at = local ? relative.toArray() : [inst.x, inst.y, inst.z ?? 0];
      body = <>
        <div className="xf-actions"><button type="button" className="btn" onClick={centreSelection}>Center on bed</button>
          <button type="button" className="btn" disabled={!canDrop()} onClick={dropSelection}>Drop to bed</button>
          <button type="button" className="btn" onClick={() => arrangeAll()}>Arrange</button></div>
        <div className="xf-grid">
          <span className="xf-key">{local ? 'Relative' : 'Position'}</span>
          {[0, 1].map((a) => <Num key={a} axis={AX[a]} label={`Position ${AX[a]}`} value={at[a]} onCommit={(val) => moveTo(a, val)} />)}
          {more ? <Num axis="Z" label="Position Z" value={at[2]} onCommit={(val) => moveTo(2, val)} /> : <span/>}<span/></div>
        <div className="xf-foot"><button type="button" className="link" aria-expanded={more} onClick={() => setMore(!more)}>{more ? 'Less' : 'More'}</button>
          {more && <CoordToggle value={coordinates} enabled onChange={(c) => { moveCoordinates = c; setCoordinates(c); }} />}</div>
      </>;
    } else if (gizmo === 'rotate') {
      const turn = (axis: number, by: number) => { const next = ((rot[axis] + by + 540) % 360) - 180;
        if (axis === 2) updateInstance(o.id, inst.id, { rotZ: next }, 'Rotate'); else updateObject(o.id, axis === 0 ? { rotX: next } : { rotY: next }, 'Rotate'); };
      const setAngle = (a: number, val: number) => (a === 2 ? updateInstance(o.id, inst.id, { rotZ: val }) : updateObject(o.id, a === 0 ? { rotX: val } : { rotY: val }));
      const back = rot.some((r, k) => Math.abs(r - start.rot[k]) > 1e-9);
      body = <>
        <div className="xf-rot">{[0, 1, 2].map((a) => <div key={a} className="xf-rot-row">
          <span className={`axis ${AX[a].toLowerCase()}`}>{AX[a]}</span>
          <button type="button" className="btn" aria-label={`Rotate ${AX[a]} by -90°`} onClick={() => turn(a, -90)}>−90°</button>
          <Num label={`Rotation ${AX[a]}`} value={rot[a]} onCommit={(val) => setAngle(a, val)} />
          <button type="button" className="btn" aria-label={`Rotate ${AX[a]} by +90°`} onClick={() => turn(a, 90)}>+90°</button>
        </div>)}</div>
        <div className="xf-actions"><button type="button" className="btn" onClick={() => setState({ gizmo: 'flatten' })}>Lay on face</button>
          <button type="button" className="btn" disabled={project} onClick={() => void toolbarOperation('orient')}>Auto orient</button>
          <button type="button" className="btn-ghost" disabled={!back && rot.every((r) => r === 0)} onClick={() => setRotation(o.id, inst.id, back ? start.rot : [0, 0, 0])}>Reset</button></div>
      </>;
    } else {
      const mesh = o.geometry.boundingBox!.getSize(new THREE.Vector3());
      const rescale = (axis: number, factor: number) => updateObject(o.id, {
        scale: o.scale.map((sc, k) => (uniform || k === axis ? sc * factor : sc)) as [number, number, number],
      });
      const pct = o.scale.map((sc) => Math.abs(sc) * 100);
      const even = pct.every((x) => Math.abs(x - pct[0]) < 1e-6), perAxis = !uniform || !even;
      const setAll = (p: number) => updateObject(o.id, { scale: o.scale.map((sc) => Math.sign(sc || 1) * p / 100) as [number, number, number] }, 'Scale');
      body = <>
        <div className="xf-grid">
                    <span className="xf-key">Scale %</span>
          {perAxis ? [0, 1, 2].map((a) => <Num key={a} axis={AX[a]} label={`Scale ${AX[a]}`} value={pct[a]} check={aboveZero} onCommit={(val) => rescale(a, val / 100 / Math.abs(o.scale[a]))} />)
            : <><Num label="Scale" value={pct[0]} check={aboveZero} onCommit={setAll} />
              <span className="seg xf-steps" role="group" aria-label="Scale steps">{[50, 100, 200].map((p) => <button key={p} type="button" aria-pressed={Math.abs(pct[0] - p) < 1e-6} onClick={() => setAll(p)}>{p}%</button>)}</span></>}
          <span/>
          <span className="xf-key">Size mm</span>
          {[0, 1, 2].map((a) => <Num key={a} axis={AX[a]} label={`Size ${AX[a]}`} value={Math.abs(o.scale[a]) * mesh.getComponent(a)}
            check={aboveZero} onCommit={(val) => rescale(a, val / Math.max(1e-6, Math.abs(o.scale[a]) * mesh.getComponent(a)))} />)}
          <span className="xf-act">{lock}</span>
        </div>
        <div className="xf-actions"><button type="button" className="btn" onClick={() => void import('../plater/objectOps').then((m) => m.scaleToBuildVolume(o.id))}>Fit to bed</button>
          <button type="button" className="btn-ghost" disabled={pct.every((x) => Math.abs(x - 100) < 1e-6)} onClick={() => setAll(100)}>Reset</button></div>
      </>;
    }
  }
  if (!active || !valid) return null;
  return <Popover.Root open onOpenChange={open => { if (!open) close(); }}>
    <Popover.Anchor asChild><div className="transform-anchor" /></Popover.Anchor>
    <InLayer portal={Popover.Portal}><Popover.Content className="xf transform-floating" aria-label={`${title} ${v?.name ?? o.name}`} side="bottom" align="start" sideOffset={0} collisionPadding={12}
      onOpenAutoFocus={e => e.preventDefault()} onCloseAutoFocus={e => e.preventDefault()}
      onInteractOutside={e => e.preventDefault()} onEscapeKeyDown={e => { e.stopPropagation(); close(); }}>
      <div className="xf-head"><span className="xf-label">{title}</span><button type="button" className="xf-fold" aria-label="Close transform" onClick={close}><Glyph name="close" size={14} /></button></div>
      <div className="xf-selection">{v?.name ?? o.name}</div>
      {v ? <div className="xf-grid">{body}</div> : body}
    </Popover.Content></InLayer>
  </Popover.Root>;
}
