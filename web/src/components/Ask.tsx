import { useState, useSyncExternalStore } from 'react';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';

type Ask = { title: string; message?: string; value?: string; range?: [number, number]; choices?: string[]; buttons?: [string, string | null]; danger?: boolean; alternative?: string; validate?(v: string): string | null; resolve(v: string | null): void };
let current: Ask | null = null;
const listeners = new Set<() => void>();
const primaryLabel = (a: Ask) => a.buttons?.[0] ?? (a.range ? 'Apply' : a.value !== undefined && !a.choices ? 'Save' : 'Continue');

export function subscribeAsk(listener: () => void) {
  listeners.add(listener);
  return () => { listeners.delete(listener); };
}

export function pendingAsk() {
  if (!current) return undefined;
  const { title, message, buttons, alternative, choices } = current;
  const options = [{ id: 'confirm', label: primaryLabel(current) }];
  if (alternative) options.push({ id: 'alternative', label: alternative });
  if (buttons?.[1] !== null) options.push({ id: 'cancel', label: buttons?.[1] ?? 'Cancel' });
  return { title, message, choices, options };
}
const show = (a: Ask | null) => {
  current = a;
  listeners.forEach((l) => l());
};
const ask = (a: Omit<Ask, 'resolve'>) =>
  new Promise<string | null>((resolve) => {
    current?.resolve(null);
    show({ ...a, resolve });
  });

export const confirmDialog = (title: string, message: string, buttons?: [string, string], danger?: boolean) => ask({ title, message, buttons, danger }).then((v) => v !== null);
export const saveDiscardDialog = (title: string, message: string, save = 'Save') =>
  ask({ title, message, buttons: [save, 'Cancel'], alternative: 'Discard' }).then((v) => v === null ? null : v === 'alternative' ? 'discard' as const : 'save' as const);
export const messageDialog = (title: string, message: string) => ask({ title, message, buttons: ['Close', null] }).then(() => undefined);
export const promptDialog = (title: string, value: string, validate?: (v: string) => string | null) => ask({ title, value, validate }).then((v) => v?.trim() || null);
export const numberDialog = (title: string, prompt: string, value: number, min: number, max: number) =>
  ask({ title, message: prompt, value: String(value), range: [min, max] }).then((v) => v === null || v.trim() === '' ? null : Math.min(max, Math.max(min, Math.round(Number(v)))))
    .then((n) => (n === null || Number.isNaN(n) ? null : n));

export const choiceDialog = (title: string, message: string, choices: string[]) =>
  ask({ title, message, value: '0', choices }).then((v) => (v === null ? null : Number(v)));

const SKIPPED: Record<string, string> = {
  settings: "the project's print settings",
  thumbnails: 'its preview images',
  painting: 'painted colours, seams and supports',
  'objects without meshes': 'objects that have no mesh',
};
export type ImportObjectsChoice = 'import' | 'settings' | 'new';

export function importObjectsPrompt(file: string, count: number, skipped: string[], allowNew: boolean, hasSettings: boolean) {
  type Option = { id: ImportObjectsChoice; label: string };
  const primary: Option = hasSettings ? { id: 'settings', label: 'Import objects and settings' } : { id: 'import', label: 'Import objects' };
  const alternative: Option | undefined = hasSettings ? { id: 'import', label: 'Import objects only' }
    : allowNew ? { id: 'new', label: 'New project from objects' } : undefined;
  const message = `${file}\n${count} ${count === 1 ? 'object' : 'objects'}${skipped.length ? `\nNot imported: ${skipped.map((s) => SKIPPED[s] ?? s).join('; ')}.` : ''}`;
  return { title: 'Import 3MF', message, primary, alternative };
}

export type ImportObjectsPrompt = ReturnType<typeof importObjectsPrompt>;

export const importObjectsDialog = ({ title, message, primary, alternative }: ImportObjectsPrompt) =>
  ask({ title, message, buttons: [primary.label, 'Cancel'], alternative: alternative?.label })
    .then((v) => v === null ? null : v === 'alternative' ? alternative?.id ?? null : primary.id);

function AskDialog({ a }: { a: Ask }) {
  const [text, setText] = useState(a.value ?? '');
  const done = (v: string | null) => {
    show(null);
    a.resolve(v);
  };
  const problem = a.value !== undefined && !a.range && !a.choices && a.validate ? a.validate(text.trim()) : null;
  const primary = primaryLabel(a);
  return (
    <Modal size="s" alert label={a.title} onClose={() => done(null)}>
      <form method="dialog" onSubmit={(e) => { e.preventDefault(); if (!problem) done(a.value === undefined ? '' : text); }}>
        <ModalTitle title={a.title} onClose={() => done(null)} />
        <ModalBody>
          {a.range ? <label className="form-row"><span>{a.message}</span>
            <input className="text num" type="number" min={a.range[0]} max={a.range[1]} step={1} value={text} onChange={(e) => setText(e.target.value)} /></label>
            : a.message && <p className="ask-message">{a.message}</p>}
          {a.choices && <div className="ask-choices" role="radiogroup" aria-label={a.title}>{a.choices.map((c, i) => (
            <label key={c} className="select-row"><input type="radio" name="ask-choice" checked={text === String(i)} onChange={() => setText(String(i))} />{c}</label>
          ))}</div>}
          {a.value !== undefined && !a.range && !a.choices && <input className="text" aria-label={a.title} aria-invalid={!!problem || undefined} value={text} onChange={(e) => setText(e.target.value)} onFocus={(e) => e.currentTarget.select()} />}
          {problem && <p className="ask-problem" role="alert">{problem}</p>}
        </ModalBody>
        <ModalFooter>
          {a.buttons?.[1] !== null && <button type="button" className="btn" onClick={() => done(null)}>{a.buttons?.[1] ?? 'Cancel'}</button>}
          {a.alternative && <button type="button" className="btn" onClick={() => done('alternative')}>{a.alternative}</button>}
          <button type="submit" className={a.danger ? 'btn-danger' : 'btn-primary'} disabled={!!problem}>{primary}</button>
        </ModalFooter>
      </form>
    </Modal>
  );
}

export function AskHost() {
  const a = useSyncExternalStore(subscribeAsk, () => current);
  return a ? <AskDialog key={a.title + a.value} a={a} /> : null;
}
