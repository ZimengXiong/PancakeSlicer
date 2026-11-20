export interface EngineWarning { code: string; object?: string; params?: Record<string, unknown>; detail?: string }

export const bareName = (name: string) => name.replace(/\.(stl|obj|3mf|amf|step|stp|drc|ply|off)$/i, '');

export const warningKey = (w: EngineWarning) => `${w.code}|${w.object ?? ''}|${w.detail ?? ''}|${JSON.stringify(w.params ?? {})}`;

export function recordWarnings(record: Record<string, unknown> | undefined): EngineWarning[] {
  const typed = Array.isArray(record?.warnings) ? record.warnings : [];
  const legacy = Array.isArray(record?.object_warnings) ? record.object_warnings : [];
  const list = [...typed, ...legacy.flatMap((w): EngineWarning[] => typeof w === 'string' && w ? [{ code: 'object_warning', detail: w }] : [])];
  const seen = new Set<string>();
  return list.flatMap((w): EngineWarning[] => {
    if (!w || typeof w !== 'object' || typeof (w as EngineWarning).code !== 'string') return [];
    const warning = w as EngineWarning, key = warningKey(warning);
    if (seen.has(key)) return [];
    seen.add(key);
    return [warning];
  });
}
