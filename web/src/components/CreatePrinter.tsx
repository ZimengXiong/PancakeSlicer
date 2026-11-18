import { Dropdown, EditableDropdown } from './Dropdown';
import { useMemo, useState } from 'react';
import { createMachine, useMachine } from '../flow/fsm';
import { NAME_MAX, nameLength, type PrinterRecord } from '../profiles/records';
import { getState, toast, useStore } from '../state/store';
import { profileInfo, recordOf } from '../profiles/view';
import { useCatalog } from '../profiles/catalog-add';
import type { CatalogModel } from '../profiles/catalog-reads';
import { catalogLine } from '../catalog/read';
import type { CatalogVariant } from '../catalog/types';
import { commitRecords } from '../profiles/transactions';
import { switchPreset } from '../actions';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';
import { vendorLabel } from '../profiles/metadata';
import { randomId } from '../random-id';

async function variantFor(model: CatalogModel, nozzle: number): Promise<CatalogVariant | undefined> {
  const near = (v: CatalogVariant) => Math.abs(Number(v.nozzle) - nozzle);
  return (await catalogLine('printers', model.id)).variants.filter((v) => v.default).sort((a, b) => near(a) - near(b))[0];
}

export default function CreatePrinter({ close }: { close(): void }) {
  const ready = useStore((st) => st.profileLibraryLoad.state === 'ready');
  const api = useStore((st) => st.api);
  const current = useStore((st) => recordOf(st, 'machine', st.printer) as PrinterRecord | undefined);
  const currentName = useStore((st) => profileInfo(st, 'machine', st.printer).name);
  const printers = useStore((st) => st.records.printers);
  const catalog = useCatalog('machine', true), vendors = catalog.data && 'vendors' in catalog.data ? catalog.data.vendors : [];
  const [fromCurrent, setFromCurrent] = useState(false);
  const [templateVendor, setTemplateVendor] = useState(current?.vendor ?? 'BBL');
  const models = vendors.find((v) => v.id === templateVendor)?.models ?? [];
  const [modelId, setModelId] = useState(current?.model ?? '');
  const model = models.find((m) => m.id === modelId) ?? models[0];
  const [vendor, setVendor] = useState('');
  const [modelName, setModelName] = useState('');
  const [nozzle, setNozzle] = useState(String(current?.nozzles[0]?.diameter ?? '0.4'));
  const diameter = Number(nozzle);
  const name = fromCurrent ? `${vendor.trim()} ${modelName.trim()}` : `${vendor.trim()} ${modelName.trim()} ${nozzle} nozzle`;
  const missing = !vendor.trim() || !modelName.trim() ? 'Please enter both a brand and a printer model name.'
    : fromCurrent && !current ? 'Please select a saved printer first.'
    : !fromCurrent && !model ? 'Pick a template printer to start from!'
    : !fromCurrent && !(diameter >= 0.1 && diameter <= 2) ? 'Pop in a nozzle diameter between 0.1 and 2 mm!'
    : nameLength(name) > NAME_MAX ? `Keep the name to ${NAME_MAX} characters (now ${nameLength(name)}).`
    : printers.some((p) => !p.archived && p.name === name) ? `“${name}” is already taken` : '';
  const intent = useMemo(() => createMachine<'idle' | 'creating', { type: 'CREATE' } | { type: 'FAILED' }, Record<string, never>>({
    id: 'create-printer', initial: 'idle', context: {},
    states: { idle: { on: { CREATE: { target: 'creating' } } }, creating: { on: { FAILED: { target: 'idle' } } } } }), []);
  const creating = useMachine(intent).value === 'creating';
  const create = async () => {
    if (missing || getState().profileLibraryLoad.state !== 'ready' || !intent.can({ type: 'CREATE' })) return;
    intent.send({ type: 'CREATE' });
    try { await write(); } catch (e) { intent.send({ type: 'FAILED' }); toast('couldnt_create_printer', { detail: e instanceof Error ? e.message : String(e) }); }
  };
  const write = async () => {
    const id = `profile:${randomId()}`;
    let record: PrinterRecord;
    if (fromCurrent) record = { ...current!, id, name, revision: 0, archived: undefined, slots: current!.slots.map((slot, i) => ({ ...slot, id: `slot:${id}:${slot.tray || i}` })) };
    else {
      const variant = await variantFor(model!, diameter);
      if (!variant) throw new Error(`Oops! ${model!.id} doesn't have a nozzle variant in the catalog.`);
      record = { id, name, revision: 0, vendor: templateVendor, model: model!.id,
        nozzles: Array.from({ length: Math.max(1, variant.extruders) }, () => ({ variant: variant.id, diameter })), slots: [] };
    }
    await commitRecords([{ kind: 'printer', record }], { label: 'Add printer' });
    switchPreset('machine', 0, id);
    toast('added_printer', { name });
    close();
  };
  const seg = <T extends string>(label: string, value: T, set: (v: T) => void, options: [T, string][]) => <div className="form-row"><span>{label}</span>
    <Dropdown className="combo" aria-label={label} value={value} onValueChange={(next) => set(next as T)} options={options.map(([v, text]) => ({ value: v, label: text }))} /></div>;
  return (
    <Modal size="m" label="Create printer" onClose={close}>
      <ModalTitle title="Create printer" onClose={close} />
      <ModalBody className="modal-fields">
        {seg('Start from', fromCurrent ? 'current' : 'template', (v) => setFromCurrent(v === 'current'), [['template', 'Template'], ['current', 'Current printer']])}
        {fromCurrent ? <div className="form-row"><span>Printer</span><span>{currentName || 'No printer selected'}</span></div> : <>
          {catalog.error && <p className="modal-error" role="alert">Oops! Couldn't read the printer catalog: {catalog.error}</p>}
          <label className="form-row"><span>Template vendor</span>
            <Dropdown className="combo" value={templateVendor} aria-label="Template vendor" disabled={!vendors.length} onValueChange={(next) => { setTemplateVendor(next); setModelId(''); }}
              options={vendors.map((v) => ({ value: v.id, label: vendorLabel({}, v.id === 'BBL' ? v.id : v.name) }))} />
          </label>
          <label className="form-row"><span>Template</span>
            <Dropdown className="combo" value={model?.id ?? ''} aria-label="Template printer" disabled={!models.length} onValueChange={setModelId} options={models.map((m) => ({ value: m.id, label: m.id }))} />
          </label>
          <label className="form-row"><span>Nozzle</span>
            <span className="modal-unit"><EditableDropdown className="text num" value={nozzle} aria-label="Nozzle diameter" onValueChange={setNozzle} onInputValueChange={setNozzle}
              options={(model?.nozzles ?? []).map((n) => ({ value: n, label: n }))} /><span>mm</span></span>
          </label>
        </>}
        <label className="form-row"><span>Vendor</span><input className="text" value={vendor} onChange={(e) => setVendor(e.target.value)} /></label>
        <label className="form-row"><span>Model</span><input className="text" value={modelName} onChange={(e) => setModelName(e.target.value)} /></label>
        <p className="hint-text">{fromCurrent ? 'A fresh copy of your current printer, settings and all.' : 'Bed and machine settings come from the template — you can tweak them anytime in your printer\'s settings.'}</p>
        {missing && (vendor.trim() || modelName.trim()) && <p className="modal-error" role="alert">{missing}</p>}
      </ModalBody>
      <ModalFooter lead={!missing && <span data-full-text={name}>{name}</span>}>
        <button type="button" className="btn" onClick={close}>Cancel</button>
        <button type="button" className="btn-primary" disabled={!!missing || !ready || !api || api.mock || creating} onClick={() => void create()}>{creating ? 'Creating…' : 'Create'}</button>
      </ModalFooter>
    </Modal>
  );
}
