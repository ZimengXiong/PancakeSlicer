import type { Gizmo, Selected } from '../state/store';
import { createMachine } from './fsm';
interface ToolsContext { selected: Selected | null; gizmo: Gizmo }
type ToolsEvent = { type: 'SYNC'; selected: Selected | null; gizmo: Gizmo } | { type: 'DRAG' } | { type: 'RELEASE' } | { type: 'CANCEL' } | { type: 'RESET' };
const sync = [
  { target: 'active', guard: (_: ToolsContext, e: Extract<ToolsEvent, { type: 'SYNC' }>) => e.gizmo !== 'none', action: (_: ToolsContext, e: Extract<ToolsEvent, { type: 'SYNC' }>) => ({ selected: e.selected, gizmo: e.gizmo }) },
  { target: 'selected', guard: (_: ToolsContext, e: Extract<ToolsEvent, { type: 'SYNC' }>) => !!e.selected, action: (_: ToolsContext, e: Extract<ToolsEvent, { type: 'SYNC' }>) => ({ selected: e.selected, gizmo: e.gizmo }) },
  { target: 'idle', action: () => ({ selected: null, gizmo: 'none' as Gizmo }) },
] as const;
const common = { SYNC: [...sync], RESET: { target: 'idle' as const, action: () => ({ selected: null, gizmo: 'none' as Gizmo }) } };
const release = [{ target: 'active' as const, guard: (c: ToolsContext) => c.gizmo !== 'none' }, { target: 'selected' as const, guard: (c: ToolsContext) => !!c.selected }, { target: 'idle' as const }];
export const tools = createMachine<'idle' | 'selected' | 'active' | 'dragging' | 'cancelled', ToolsEvent, ToolsContext>({
  id: 'tools', initial: 'idle', context: { selected: null, gizmo: 'none' },
  states: {
    idle: { on: common }, selected: { on: { ...common, DRAG: 'dragging' } },
    active: { on: { ...common, DRAG: 'dragging' } },
    dragging: { on: { ...common,
      SYNC: [{ guard: (c, e) => c.selected === e.selected && c.gizmo === e.gizmo }, ...sync],
      RELEASE: release,
      CANCEL: 'cancelled',
    } },
    cancelled: { on: { ...common, RELEASE: release } },
  },
});
