import { useState } from 'react';
import type { LayerRec } from '../preview/format';
import { filamentColor } from '../plater/filamentColor';
import { effectiveConfig, getState, setEvents, useStore, type LayerEvent } from '../state/store';
import type { SliderTick } from './PreviewSliders';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';
import { ActionMenu, type MenuEntry } from './Menu';
import { SEP } from '../separator';
import { slotFilaments } from '../profiles/slots';

const COLOR: Record<LayerEvent['type'], string> = {
  pause: '#E67E22', custom: '#3A7BD5', template: '#8E44AD', colour: '#E53935', change: '#00AE42',
};

export function eventLabel(e: LayerEvent): string {
  switch (e.type) {
    case 'pause': return 'Pause';
    case 'custom': return 'Custom G-code';
    case 'template': return 'Custom template';
    case 'colour': return 'Color change';
    case 'change': return `Change filament ${e.text}`;
  }
}

const sameZ = (a: number, b: number) => Math.abs(a - b) < 1e-3;

export function eventTicks(events: LayerEvent[], layers: LayerRec[]): SliderTick[] {
  return events.flatMap((e) => {
    const i = layers.findIndex((l) => l.z >= e.z - 1e-3);
    return i < 0 ? [] : [{ layer: i, color: COLOR[e.type], title: `${eventLabel(e)} at ${e.z.toFixed(2)} mm${e.text && e.type === 'custom' ? `: ${e.text}` : ''}` }];
  });
}

export function EventMenu({ anchor, z, layer, onClose, onJump }: { anchor: DOMRect; z: number; layer: number; onClose(): void; onJump?(): void }) {
  const events = useStore((s) => s.events);
  const slots = useStore((s) => slotFilaments(s).length);
  const template = useStore((s) => String(effectiveConfig(s, 'machine').template_custom_gcode ?? ''));
  const [custom, setCustom] = useState<string | null>(null);
  const here = events.filter((e) => sameZ(e.z, z));
  const put = (e: LayerEvent) => {
    setEvents([...getState().events.filter((x) => !sameZ(x.z, z)), e], `Add ${eventLabel(e).toLowerCase()}`);
    onClose();
  };
  const del = () => {
    setEvents(getState().events.filter((x) => !sameZ(x.z, z)), 'Delete layer event');
    onClose();
  };
  if (custom !== null)
    return (
      <Modal size="m" label="Custom G-code" onClose={onClose} returnFocus=".ls-plus">
          <ModalTitle title={`Custom G-code at ${z.toFixed(2)} mm`} onClose={onClose} />
          <ModalBody>
            <textarea className="text multiline gcode-input" aria-label={`Custom G-code at ${z.toFixed(2)} mm`} value={custom} spellCheck={false} onChange={(e) => setCustom(e.target.value)} />
          </ModalBody>
          <ModalFooter>
            <button type="button" className="btn" onClick={onClose}>Cancel</button>
            <button type="button" className="btn-primary" disabled={!custom.trim()} onClick={() => put({ z, type: 'custom', text: custom })}>Save</button>
          </ModalFooter>
      </Modal>
    );

  const changeItems: MenuEntry[] = Array.from({ length: slots }, (_, i) => ({ label: `Filament ${i + 1}`,
    icon: <span className="menu-swatch" style={{ background: filamentColor(getState(), i + 1) }} />,
    action: () => put({ z, type: 'change', text: String(i + 1) }) }));
  const items: MenuEntry[] = [{ label: `Layer ${layer + 1}${SEP}${z.toFixed(2)} mm`, header: true }];
  if (here.length) {
    if (here[0].type === 'custom') items.push({ label: 'Edit Custom G-code', keepOpen: true, action: () => setCustom(here[0].text ?? '') });
    if (here[0].type === 'change' && slots > 1) items.push(...changeItems);
    items.push({ label: `Delete ${eventLabel(here[0]).replace(/ \d+$/, '')}`, action: del });
  } else {
    if (slots === 1) items.push({ label: 'Add Color Change', action: () => put({ z, type: 'colour' }) });
    items.push({ label: 'Add Pause', action: () => put({ z, type: 'pause' }) },
      { label: 'Add Custom G-code', keepOpen: true, action: () => setCustom('') });
    if (template.trim()) items.push({ label: 'Add Custom Template', action: () => put({ z, type: 'template' }) });
    if (onJump) items.push({ label: 'Jump to layer', action: onJump });
    if (slots > 1) items.push({ label: 'Change Filament', header: true }, ...changeItems);
  }
  return <ActionMenu label={`Layer ${layer + 1}`} at={{ x: anchor.left, y: anchor.top }} items={items} onClose={onClose}
    returnFocus={document.querySelector<HTMLElement>('.ls-plus')} />;
}

export function EventList({ onPick }: { onPick?(z: number): void }) {
  const events = useStore((s) => s.events);
  if (!events.length) return null;
  return (
    <div className="events-panel">
      <div className="events-title">Custom G-code</div>
      {events.map((e) => (
        <div key={`${e.z}-${e.type}`} className="events-row">
          <span className="events-dot" style={{ background: COLOR[e.type] }} />
          <button className="link events-z" onClick={() => onPick?.(e.z)}>
            {e.z.toFixed(2)} mm
          </button>
          <span className="events-label">
            {eventLabel(e)}
            {e.type === 'custom' && e.text ? `: ${e.text.split('\n')[0]}` : ''}
          </span>
          <button className="icon-btn" aria-label="Delete layer event" onClick={() => setEvents(getState().events.filter((x) => x !== e), 'Delete layer event')}>
            ×
          </button>
        </div>
      ))}
    </div>
  );
}
