export interface EngineWarning { code: string; object?: string; params?: Record<string, unknown>; detail?: string }

export const bareName = (name: string) => name.replace(/\.(stl|obj|3mf|amf|step|stp|drc|ply|off)$/i, '');

export const warningKey = (w: EngineWarning) => `${w.code}|${w.object ?? ''}|${JSON.stringify(w.params ?? {})}`;

export function recordWarnings(record: Record<string, unknown> | undefined): EngineWarning[] {
  const list = Array.isArray(record?.warnings) ? record.warnings : [];
  const seen = new Set<string>();
  return list.flatMap((w): EngineWarning[] => {
    if (!w || typeof w !== 'object' || typeof (w as EngineWarning).code !== 'string') return [];
    const warning = w as EngineWarning, key = warningKey(warning);
    if (seen.has(key)) return [];
    seen.add(key);
    return [warning];
  });
}
