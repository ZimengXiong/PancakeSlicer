import { OPTIONS } from './options';
import { bareName, type EngineWarning } from './engine-warnings';

const n = (v: unknown) => Number(v).toLocaleString('en-US');
const motion = (p: Record<string, unknown>, what: string) => {
  const name = optionName(p.option);
  return name.toLowerCase().includes(what) ? name : `${name} ${what}`;
};
const capped = (p: Record<string, unknown>, what: string) => `${motion(p, what)} is capped to the printer's limit${p.limit != null ? ` (${p.limit})` : ''}`;
const optionName = (key: unknown) => typeof key === 'string' ? OPTIONS[key]?.full_label || OPTIONS[key]?.label || key : 'A setting';
const WARNING_TEXTS: Record<string, ((p: Record<string, unknown>) => string) | null> = {
  curved_tops_head_strike_planar: () => 'Some curved areas were kept flat so the print head stays clear',
  curved_tops_all_struck_planar: () => 'Printed with flat layers because the print head would bump into every curved top',
  curved_tops_restored_planar: () => 'Printed with flat layers because no curved top was left',
  curved_tops_zaa_flat: () => 'Tops printed flat instead of contoured so the print head stays clear',
  curved_top_flat_share: (p) => `${n(p.percent)}% of the curved top prints with flat layers`,
  curved_shells_only: () => 'Only the top shells are curved; the whole part could not be',
  conformal_flat_fallback: (p) => `Printed with flat layers so the print head doesn't bump the part (${n(p.moves)} potential collisions avoided)`,
  conformal_stepped_top: (p) => `${n(p.area_mm2)} mm² of the curved top steps between layers—a smaller layer height prints it smooth`,
  smooth_morph_flat: null,
  smooth_morph_limited: () => 'Smooth morph was limited on this part',
  auto_morph_to_curved_tops: () => 'Auto printed Curved tops so the nozzle stays clear',
  auto_printed_curved: () => 'Auto printed it curved',
  auto_switched_flat: (p) => `Switched from ${p.method === 'oneshot' ? 'One-shot' : 'Auto'} to flat layers`,
  nozzle_contacts_planar: () => 'Printed with flat layers and support so the nozzle stays clear',
  slant_flat: () => 'Slant printed it with flat layers',
  multiplanar_auto: () => 'Auto printed it in several planes',
  rotary_planar: () => 'Printed with flat layers on the rotary axis',
  sublayer_walls_skipped: () => 'Sub-layer walls were skipped',
  atom_toolpath_note: () => 'Note from the imported toolpath',
  head_profile_fallback: () => "This printer's head shape isn't known, so a generic one was used for clearance",
  clearance_hits: (p) => `${n(p.moves)} moves would hit the printed part`,
  clearance_repaired: () => 'Curved moves were adjusted so the print head stays clear',
  clearance_repair_summary: () => 'Curved moves were adjusted so the print head stays clear',
  clearance_residual_hits: (p) => `${n(p.moves)} curved moves still brush the printed part`,
  wave_hits: (p) => `${n(p.moves)} wave moves would hit the printed part`,
  mesh_repaired: () => 'Auto-repaired mesh on import',
  floating_regions: () => 'Floating overhangs detected—try rotating the model or turning on supports',
  floating_cantilever: () => 'Floating cantilever detected—try rotating the model or turning on supports',
  large_overhangs: () => 'Large overhangs detected—try rotating the model or turning on supports',
  may_need_support: () => 'This may need support',
  support_added: (p) => `Support added so it doesn't float (${n(p.volume_mm3)} mm³)`,
  empty_layers: () => 'Some layers print nothing—the model may be paper-thin there or have a broken mesh',
  xy_compensation_ignored_color: () => "XY size compensation isn't used on color-painted objects",
  xy_compensation_ignored_fuzzy: () => "XY size compensation isn't used on fuzzy-skin-painted objects",
  brim_clipped: () => 'Part of the brim was clipped at the bed edge—move the objects inward for a full brim',
  priming_region_close: () => 'The print is very close to the priming area—make sure nothing collides',
  precise_seam: () => 'Precise seam could not be used everywhere',
  belt_purge_overflow: () => "The belt purge tower can't absorb all the purge—widen it or reduce flushing volumes",
  imex_mode_fallback: () => 'Printing in Primary mode: the chosen tool mode has no tools assigned',
  temperature_lines_suspect: () => 'Some temperature commands in the G-code may be wrong',
  outside_print_volume: (p) => p.part === 'prime_tower' ? 'The prime tower is partly outside the printable area' : 'Part of the print is taller than the printer can reach',
  settings_warning: null,
  jerk_capped: (p) => capped(p, 'jerk'),
  acceleration_capped: (p) => capped(p, 'acceleration'),
  junction_deviation_capped: (p) => capped(p, 'junction deviation'),
  jerk_low: (p) => `${motion(p, 'jerk')} is very low, which can leave ripples or blobs on curved walls`,
  sla_drilling_failed: () => "The holes couldn't be drilled into the model",
  sla_drilling_partial: () => "Some holes couldn't be drilled into the model",
  sla_hollowing_unsuitable: () => "This model can't be hollowed",
};
const WITH_DETAIL = new Set(['curved_top_flat_share', 'curved_shells_only', 'smooth_morph_limited', 'auto_printed_curved', 'auto_switched_flat',
  'slant_flat', 'multiplanar_auto', 'rotary_planar', 'sublayer_walls_skipped', 'atom_toolpath_note', 'may_need_support', 'settings_warning',
  'precise_seam', 'imex_mode_fallback', 'settings_warning', 'clearance_hits', 'clearance_repair_summary', 'clearance_residual_hits', 'head_profile_fallback']);

export function warningText(w: EngineWarning): string | null {
  if (!(w.code in WARNING_TEXTS)) return `The slicer reported a warning (${w.code.replace(/_/g, ' ')})`;
  return WARNING_TEXTS[w.code]?.(w.params ?? {}) ?? null;
}
const keepsDetail = (w: EngineWarning) => !(w.code in WARNING_TEXTS) || WITH_DETAIL.has(w.code);
export interface PlainWarning {
  object?: string;
  code: string;
  text: string | null;
  detail?: string;
}

export function plainWarnings(warnings: EngineWarning[], objectNames: string[]): PlainWarning[] {
  const names = new Set(objectNames.map(bareName).filter(Boolean));
  return warnings.map((w) => {
    const object = w.object && names.has(bareName(w.object)) ? bareName(w.object) : w.object;
    return { ...(object ? { object } : {}), code: w.code, text: warningText(w), ...(keepsDetail(w) && w.detail ? { detail: w.params?.option ? `${optionName(w.params.option)}: ${w.detail}` : w.detail } : {}) };
  });
}
