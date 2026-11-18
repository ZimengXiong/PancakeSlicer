import { RAIL_MIN, railMax, setRailWidth } from '../sidebar';

export function RailResizer() {
  const start = (e: React.PointerEvent<HTMLDivElement>) => {
    if (e.button !== 0) return;
    const rail = document.querySelector<HTMLElement>('.main .sidebar');
    if (!rail) return;
    e.preventDefault();
    const left = rail.getBoundingClientRect().left, target = e.currentTarget;
    target.setPointerCapture(e.pointerId);
    document.body.classList.add('rail-resizing');
    let last = rail.getBoundingClientRect().width;
    const move = (ev: PointerEvent) => { last = Math.min(railMax(), Math.max(RAIL_MIN, ev.clientX - left)); setRailWidth(last, false); };
    const up = () => { target.removeEventListener('pointermove', move); target.removeEventListener('pointerup', up); target.removeEventListener('pointercancel', up);
      document.body.classList.remove('rail-resizing'); setRailWidth(last, true); };
    target.addEventListener('pointermove', move); target.addEventListener('pointerup', up); target.addEventListener('pointercancel', up);
  };
  return <div className="rail-resizer" role="separator" aria-orientation="vertical" aria-label="Resize sidebar" title="Drag to resize, or double-click to snap back to default width"
    onPointerDown={start} onDoubleClick={() => setRailWidth(null)} />;
}
