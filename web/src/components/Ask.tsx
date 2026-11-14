import { useState, useSyncExternalStore } from 'react';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';

type Ask = { title: string; message?: string; value?: string; range?: [number, number]; choices?: string[]; buttons?: [string, string | null]; danger?: boolean; alternative?: string; resolve(v: string | null): void };
let current: Ask | null = null;
const listeners = new Set<() => void>();
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
export const messageDialog = (title: string, message: string, button = 'Close') => ask({ title, message, buttons: [button, null] }).then(() => undefined);
export const promptDialog = (title: string, value: string) => ask({ title, value }).then((v) => v?.trim() || null);
export const numberDialog = (title: string, prompt: string, value: number, min: number, max: number) =>
  ask({ title, message: prompt, value: String(value), range: [min, max] }).then((v) => v === null || v.trim() === '' ? null : Math.min(max, Math.max(min, Math.round(Number(v)))))
    .then((n) => (n === null || Number.isNaN(n) ? null : n));

export const choiceDialog = (title: string, message: string, choices: string[]) =>
  ask({ title, message, value: '0', choices }).then((v) => (v === null ? null : Number(v)));

export const importObjectsDialog = (file: string, count: number, skipped: string[], allowNew: boolean) =>
  ask({ title: 'Import 3MF', message: `${file}\n${count} ${count === 1 ? 'object' : 'objects'}${skipped.length ? `\nSkipped: ${skipped.join(', ')}.` : ''}`,
    buttons: ['Import objects', 'Cancel'], ...(allowNew ? { alternative: 'New project from objects' } : {}) })
    .then((v) => v === null ? null : v === 'alternative' ? 'new' : 'import');

function AskDialog({ a }: { a: Ask }) {
  const [text, setText] = useState(a.value ?? '');
  const done = (v: string | null) => {
    show(null);
    a.resolve(v);
  };
  const primary = a.buttons?.[0] ?? (a.range ? 'Apply' : a.value !== undefined && !a.choices ? 'Save' : 'Continue');
  return (
    <Modal size="s" alert label={a.title} onClose={() => done(null)}>
      <form method="dialog" onSubmit={(e) => { e.preventDefault(); done(a.value === undefined ? '' : text); }}>
        <ModalTitle title={a.title} onClose={() => done(null)} />
        <ModalBody>
          {a.range ? <label className="form-row"><span>{a.message}</span>
            <input className="text num" type="number" min={a.range[0]} max={a.range[1]} step={1} value={text} onChange={(e) => setText(e.target.value)} /></label>
            : a.message && <p className="ask-message">{a.message}</p>}
          {a.choices && <div className="ask-choices" role="radiogroup" aria-label={a.title}>{a.choices.map((c, i) => (
            <label key={c} className="select-row"><input type="radio" name="ask-choice" checked={text === String(i)} onChange={() => setText(String(i))} />{c}</label>
          ))}</div>}
          {a.value !== undefined && !a.range && !a.choices && <input className="text" aria-label={a.title} value={text} onChange={(e) => setText(e.target.value)} onFocus={(e) => e.currentTarget.select()} />}
        </ModalBody>
        <ModalFooter>
          {a.buttons?.[1] !== null && <button type="button" className="btn" onClick={() => done(null)}>{a.buttons?.[1] ?? 'Cancel'}</button>}
          {a.alternative && <button type="button" className="btn" onClick={() => done('alternative')}>{a.alternative}</button>}
          <button type="submit" className={a.danger ? 'btn-danger' : 'btn-primary'}>{primary}</button>
        </ModalFooter>
      </form>
    </Modal>
  );
}

export function AskHost() {
  const a = useSyncExternalStore((l) => { listeners.add(l); return () => void listeners.delete(l); }, () => current);
  return a ? <AskDialog key={a.title + a.value} a={a} /> : null;
}
