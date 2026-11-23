

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
  onLaunchFilesAvailable(cb: () => void) {
    const notify = () => cb();
    ipcRenderer.on('orca-desktop:launch-files-available', notify);
    return () => { ipcRenderer.removeListener('orca-desktop:launch-files-available', notify); };
  },
  readLaunchFiles: (after: number) => ipcRenderer.invoke('orca-desktop:read-launch-files', after),
  acceptLaunchFile: (id: number, index: number): Promise<void> => ipcRenderer.invoke('orca-desktop:accept-launch-file', id, index),
  showDeviceUi: (address: string | null, apiAddress?: string): Promise<void> => ipcRenderer.invoke('orca-desktop:device-ui', address, apiAddress),
  agents: {
    get: () => ipcRenderer.invoke('orca-desktop:agents'),
    set: (enabled: boolean, printers: boolean) => ipcRenderer.invoke('orca-desktop:agents-set', enabled, printers),
    client: (id: string, add: boolean) => ipcRenderer.invoke('orca-desktop:agents-client', id, add),
    onCommand(handler: (id: number, command: string, args: Record<string, unknown>) => void) {
      const listener = (_e: Electron.IpcRendererEvent, id: number, command: string, args: Record<string, unknown>) => handler(id, command, args);
      ipcRenderer.on('orca-desktop:agent-command', listener);
      return () => { ipcRenderer.removeListener('orca-desktop:agent-command', listener); };
    },
    reply: (id: number, ok: boolean, result: unknown): Promise<void> => ipcRenderer.invoke('orca-desktop:agent-reply', id, ok, result),
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
