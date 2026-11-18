export const plainName = (name: string) => name.replace(/\.(stl|obj|3mf|drc|step|stp|amf|ply)$/i, '');
export function objectLabels(objects: readonly { id: number; name: string }[]): Map<number, string> {
  const seen = new Map<string, number>(), out = new Map<number, string>();
  for (const o of objects) {
    const base = plainName(o.name), n = (seen.get(base) ?? 0) + 1;
    seen.set(base, n);
    out.set(o.id, n > 1 ? `${base} (${n})` : base);
  }
  return out;
}
