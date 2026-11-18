import type { OptionDef } from '../options';

export function fieldNumber(value: string | number, def: OptionDef): string {
  const text = String(value).trim();
  if (!text) return 'Empty value';
  const side = def.sidetext?.trim();
  const unit = text.endsWith('%') ? '' : side?.replace(/ or %$/, '');
  return `${text}${unit ? `${/^[°%]/.test(unit) ? '' : ' '}${unit}` : ''}`;
}

const unbounded = (v: number | undefined) => v === undefined || !Number.isFinite(Number(v)) || Math.abs(Number(v)) >= 3.4e38;

export function fieldRange(def: OptionDef): string | undefined {
  if (!/^(?:float|int|percent)/.test(def.type)) return undefined;
  def = { ...def, min: unbounded(def.min) ? undefined : def.min, max: unbounded(def.max) ? undefined : def.max };
  const alternative = def.type.includes('_or_percent') ? ' (or %)' : '';
  if (def.min !== undefined && def.max !== undefined) return `Allowed range: ${fieldNumber(def.min, def)} to ${fieldNumber(def.max, def)}${alternative}`;
  if (def.min !== undefined) return `Minimum: ${fieldNumber(def.min, def)}${alternative}`;
  if (def.max !== undefined) return `Maximum: ${fieldNumber(def.max, def)}${alternative}`;
  return undefined;
}

export function fieldIssue(text: string, def: OptionDef): { inline: string; detail: string } | undefined {
  const raw = text.trim(), percent = raw.endsWith('%'), number = percent ? raw.slice(0, -1).trim() : raw;
  const n = Number(number);
  if (!/^[+-]?(?:\d+\.?\d*|\.\d+)(?:e[+-]?\d+)?$/i.test(number) || !Number.isFinite(n))
    return { inline: 'Enter a number', detail: 'isn\'t a valid number' };
  if (def.type.startsWith('int') && !percent && !/^[+-]?\d+$/.test(number)) return { inline: 'Enter a whole number', detail: 'needs to be a whole number' };
  if (percent && !def.type.includes('percent')) return { inline: 'Use a number without %', detail: 'takes a plain number, not a percentage (%)' };
  const limit = (value: number) => fieldNumber(`${value}${percent ? '%' : ''}`, def);
  if (def.min !== undefined && n < def.min) return { inline: `Min ${limit(def.min)}`, detail: `is below the minimum (${limit(def.min)})` };
  if (def.max !== undefined && n > def.max) return { inline: `Max ${limit(def.max)}`, detail: `is above the maximum (${limit(def.max)})` };
  if (def.type.startsWith('int') && !Number.isInteger(n)) return { inline: 'Enter a whole number', detail: 'needs to be a whole number' };
  return undefined;
}
