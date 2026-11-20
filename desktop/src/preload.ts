

import { contextBridge, ipcRenderer } from 'electron';
const listeners = new Set<(command: string) => void>();
const beforeClose = new Set<() => Promise<void>>();
let markCloseReady: () => void;
const closeReady = new Promise<void>((resolve) => { markCloseReady = resolve; });

const editable = (el: Element | null) =>
  !!el && (el instanceof HTMLTextAreaElement || (el instanceof HTMLInputElement && !['button', 'checkbox', 'radio', 'range', 'file'].includes(el.type))
    || (el as HTMLElement).isContentEditable);

ipcRenderer.on('orca-desktop:menu', (_e, command: string) => {

  if ((command === 'undo' || command === 'redo') && editable(document.activeElement)) {
    document.execCommand(command);
    return;
  }
  window.dispatchEvent(new CustomEvent('orca-desktop-menu', { detail: command }));
  for (const cb of listeners) cb(command);
});

contextBridge.exposeInMainWorld('orcaDesktop', {
  version: 1,
  platform: process.platform,


  bambuConnect: (jobId: string, filename: string): Promise<void> => ipcRenderer.invoke('orca-desktop:bambu-connect', jobId, filename),
  onLaunchFiles(cb: (files: { name: string; data: Uint8Array<ArrayBuffer>; lastModified: number }[]) => Promise<void>) {
    let active = true;
    let draining = Promise.resolve();
    const drain = () => {
      draining = draining.then(async () => {
        const files = await ipcRenderer.invoke('orca-desktop:take-launch-files') as { name: string; data: Uint8Array<ArrayBuffer>; lastModified: number }[];
        if (active && files.length) await cb(files);
      }).catch(console.error);
    };
    ipcRenderer.on('orca-desktop:launch-files-available', drain);
    drain();
    return () => { active = false; ipcRenderer.removeListener('orca-desktop:launch-files-available', drain); };
  },
  onBeforeClose(handler: () => Promise<void>) {
    beforeClose.add(handler);
    markCloseReady();
    return () => { beforeClose.delete(handler); };
  },
  async flushBeforeClose() {

    await closeReady;
    await Promise.all([...beforeClose].map((handler) => handler()));
  },
  onMenu(cb: (command: string) => void) {
    listeners.add(cb);
    return () => { listeners.delete(cb); };
  },
});
