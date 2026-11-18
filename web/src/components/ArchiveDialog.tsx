import { Check } from './Check';
import { useState } from 'react';
import { unzipSync } from 'fflate';
import { importFiles } from '../actions';
import { toast } from '../state/store';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';

const modelPattern = () => new RegExp(`\\.(stl|obj|3mf|step|stp)$`, 'i');

export function importZipArchive(open: (files: File[]) => void) {
  const input = document.createElement('input');
  input.type = 'file';
  input.accept = '.zip';
  input.onchange = async () => {
    const zip = input.files?.[0];
    if (!zip) return;
    try {
      const pattern = modelPattern();
      const entries = unzipSync(new Uint8Array(await zip.arrayBuffer()), { filter: (f) => pattern.test(f.name) && !f.name.startsWith('__MACOSX/') });
      const files = Object.entries(entries).sort(([a], [b]) => a.localeCompare(b))
        .map(([path, data]) => Object.assign(new File([data as BlobPart], path.split('/').pop()!), { path }));
      if (!files.length) return toast('didnt_find_any_3d');
      open(files);
    } catch (e) {
      toast('couldnt_open_archive', { detail: e instanceof Error ? e.message : String(e) });
    }
  };
  input.click();
}

export default function ArchiveDialog({ files, close }: { files: (File & { path?: string })[]; close(): void }) {
  const [checked, setChecked] = useState(() => files.map(() => true));
  const count = checked.filter(Boolean).length;
  return (
    <Modal size="m" label="Import from archive" onClose={close}>
      <ModalTitle title="Import from archive" onClose={close} />
      <ModalBody>
        <div className="modal-pick">
          <div className="modal-pick-head">
            <button type="button" className="link" onClick={() => setChecked(files.map(() => true))}>Select all</button>
            <button type="button" className="link" onClick={() => setChecked(files.map(() => false))}>Clear</button>
          </div>
          <div className="modal-pick-list">{files.map((f, i) => (
            <label key={i} className="modal-check"><Check checked={checked[i]} onCheckedChange={(next) => setChecked(checked.map((c, j) => (j === i ? next : c)))} /><span data-full-text={f.path ?? f.name}>{f.path ?? f.name}</span></label>
          ))}</div>
        </div>
      </ModalBody>
      <ModalFooter lead={`${count} of ${files.length}`}>
        <button type="button" className="btn" onClick={close}>Cancel</button>
        <button type="button" className="btn-primary" disabled={!count} onClick={() => { close(); void importFiles(files.filter((_, i) => checked[i])); }}>{count === 1 ? 'Import 1 file' : `Import ${count} files`}</button>
      </ModalFooter>
    </Modal>
  );
}
