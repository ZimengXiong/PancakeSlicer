
import { app, BrowserWindow, dialog, ipcMain, Menu, session, shell, type MenuItemConstructorOptions } from 'electron';
import { spawn, type ChildProcess } from 'node:child_process';
import { createHash, randomBytes } from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { ProjectStore } from './projects';
import { LegacyWorkspaceSource, type LegacyRead } from './legacy-workspace';
import { DESKTOP_ORIGIN, LegacyRendererSource, type LegacyRendererPartRead } from './legacy-renderer';

const MAC = process.platform === 'darwin';
const EXE = process.platform === 'win32' ? '.exe' : '';

let server: ChildProcess | null = null;
let origin = '';
let win: BrowserWindow | null = null;
let quitting = false;
let quitReady = false;
let closing = false;
let projects: ProjectStore | null = null;



app.commandLine.appendSwitch('enable-unsafe-swiftshader');
for (const name of [
  'disable-background-networking', 'disable-component-update', 'disable-domain-reliability',
  'disable-sync', 'disable-breakpad', 'disable-client-side-phishing-detection',
  'no-proxy-server', 'disable-quic',
]) app.commandLine.appendSwitch(name);
app.commandLine.appendSwitch('disable-features',
  'AutofillServerCommunication,CertificateTransparencyComponentUpdater,OptimizationHints,MediaRouter,SpellingServiceClient');




// Projects live in the OS app-data dir under the product's name. Data from builds that used "OrcaSlicer Web" moves
// over once, before anything opens it. Walks and scratch runs use their own profile.
function productUserData(): string {
  const appData = app.getPath('appData'), current = path.join(appData, 'PancakeSlicer'), old = path.join(appData, 'OrcaSlicer Web');
  if (!fs.existsSync(current) && fs.existsSync(old)) fs.renameSync(old, current);
  fs.rmSync(path.join(appData, 'OrcaSlicer Web Cache'), { recursive: true, force: true });
  return current;
}
app.setPath('userData', process.env.ORCA_DESKTOP_USER_DATA ?? productUserData());



const originalUserData = app.getPath('userData');
const originalSessionData = app.getPath('sessionData');
const legacyRenderer = new LegacyRendererSource(originalUserData, originalSessionData, () =>



  process.versions.electron === '45.2.1' && (process.platform === 'linux' || process.platform === 'darwin')
  && app.hasSingleInstanceLock() && app.getPath('userData') === originalUserData
  && originalSessionData === originalUserData
  && !['user-data-dir', 'profile-directory', 'session-data-dir'].some((name) => app.commandLine.hasSwitch(name)));
const sessionCacheRoot = path.join(app.getPath('appData'), 'PancakeSlicer Cache');
let ownedSessionDirectory: { path: string; dev: number; ino: number } | null = null;

function cleanSessionDirectory() {
  const owned = ownedSessionDirectory;
  if (!owned) return;
  ownedSessionDirectory = null;
  try {
    const stat = fs.lstatSync(owned.path);
    if (!stat.isDirectory() || stat.isSymbolicLink() || stat.dev !== owned.dev || stat.ino !== owned.ino) return;
    fs.rmSync(owned.path, { recursive: true, force: true });
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code !== 'ENOENT') console.error('Disposable Chromium cache cleanup did not finish.');
  }
}

try {
  fs.mkdirSync(sessionCacheRoot, { recursive: true, mode: 0o700 });
  const directory = fs.mkdtempSync(path.join(sessionCacheRoot, 'session-'));
  const stat = fs.lstatSync(directory);
  if ([originalUserData, originalSessionData].some((old) => path.resolve(old) === directory))
    throw new Error('Oops! The app needs a separate folder for browser session data and can\'t use your main storage folder directly.');
  ownedSessionDirectory = { path: directory, dev: stat.dev, ino: stat.ino };
  app.setPath('sessionData', directory);
} catch {
  cleanSessionDirectory();
  throw new Error('Oops! We couldn\'t set up a clean browser session, so the app window couldn\'t start.');
}
process.once('exit', cleanSessionDirectory);

if (!app.requestSingleInstanceLock()) {
  app.quit();
} else {
  app.on('second-instance', () => {
    if (!win) return;
    if (win.isMinimized()) win.restore();
    win.show();
    win.focus();
  });
  app.whenReady().then(start).catch((e) => fail(String(e?.stack ?? e)));
}


function payload() {
  if (app.isPackaged) return { web: path.join(process.resourcesPath, 'web'), engine: path.join(process.resourcesPath, 'engine') };
  const build = path.join(__dirname, '..', 'build');
  const os = MAC ? 'mac' : process.platform === 'win32' ? 'win' : 'linux';
  return { web: path.join(build, 'web'), engine: process.env.ORCA_DESKTOP_ENGINE ?? path.join(build, `engine-${os}-${process.arch}`) };
}

interface BundleIdentity {
  client_source: string;
  server_source: string;
  native_source: string;
  native_binary_sha256: string;
  native_resources_sha256: string;
  server_binary_sha256: string;
  native_manifest_sha256: string;
  native_qualification_sha256: string;

  schemas: Record<string, number>;
}

async function fileHash(file: string) {
  const hash = createHash('sha256');
  for await (const chunk of fs.createReadStream(file)) hash.update(chunk);
  return hash.digest('hex');
}

async function bundleIdentity(web: string, engine: string): Promise<BundleIdentity> {
  const expected = JSON.parse(fs.readFileSync(path.join(engine, 'COMPONENTS.json'), 'utf8')) as BundleIdentity;
  const webIdentity = JSON.parse(fs.readFileSync(path.join(web, 'COMPONENTS.json'), 'utf8')) as BundleIdentity;
  for (const key of ['client_source', 'server_source', 'native_source'] as const) {
    if (!/^[a-f0-9]{40}$/.test(expected[key]) || expected[key] !== webIdentity[key]) throw new Error('Oops! This app bundle is missing some build info or looks corrupted. Try reinstalling PancakeSlicer.');
  }
  if (fs.readFileSync(path.join(web, 'REVISION.txt'), 'utf8').trim() !== expected.client_source) throw new Error('Oops! The bundled web interface doesn\'t match this app version. Try reinstalling PancakeSlicer.');
  const hashes = await Promise.all(([
    [`orca-webapp-server${EXE}`, 'server_binary_sha256'], [`nonplanar_slice_tool${EXE}`, 'native_binary_sha256'],
    ['surface-manifest.json', 'native_manifest_sha256'], ['artifact-qualification.ready', 'native_qualification_sha256'],
  ] as const).map(async ([name, key]) => /^[a-f0-9]{64}$/.test(expected[key]) && await fileHash(path.join(engine, name)) === expected[key]));
  if (!hashes.every(Boolean)) throw new Error('Oops! One of the app\'s bundled files looks modified or corrupted. Try reinstalling PancakeSlicer.');
  if (!/^[a-f0-9]{64}$/.test(expected.native_resources_sha256)) throw new Error('Oops! Required app resources are missing from this installation. Try reinstalling PancakeSlicer.');
  const schemas = JSON.parse(fs.readFileSync(path.join(engine, 'schemas.json'), 'utf8')) as Record<string, number>;
  if (!Object.keys(schemas).length || !Object.values(schemas).every((v) => Number.isInteger(v) && v > 0)) throw new Error('Oops! Bundled server configuration files are missing. Try reinstalling PancakeSlicer.');
  return { ...expected, schemas };
}

function healthy(nonce: string, expected: BundleIdentity): Promise<boolean> {
  return new Promise((resolve) => {
    const req = http.get(`${DESKTOP_ORIGIN}/api/healthz`, { timeout: 1000, headers: { 'Cache-Control': 'no-store' } }, (res) => {
      if (res.statusCode !== 200) { res.resume(); resolve(false); return; }
      let body = '';
      res.setEncoding('utf8');
      res.on('data', (chunk: string) => {
        body += chunk;
        if (body.length > 65536) { req.destroy(); resolve(false); }
      });
      res.on('error', () => resolve(false));
      res.on('end', () => {
        try {
          const health = JSON.parse(body);
          const artifact = health.artifact_manifest;
          resolve(health.desktop_ready_nonce === nonce && health.server_revision === expected.server_source
            && Object.entries(expected.schemas).every(([key, value]) => health[key] === value)
            && artifact?.publication_main_sha === expected.server_source && artifact.source_sha === expected.native_source && artifact.binary_sha256 === expected.native_binary_sha256
            && artifact.resources_sha256 === expected.native_resources_sha256);
        } catch { resolve(false); }
      });
    });
    req.on('error', () => resolve(false));
    req.on('timeout', () => { req.destroy(); resolve(false); });
  });
}

async function startServer(): Promise<string> {
  const { web, engine } = payload();
  const expected = await bundleIdentity(web, engine);
  const data = app.getPath('userData');
  const logs = path.join(data, 'logs');
  fs.mkdirSync(logs, { recursive: true });
  // The previous launch's log stays as server.prev.log, so a crash can still be reported after a restart.
  const current = path.join(logs, 'server.log');
  if (fs.existsSync(current)) fs.renameSync(current, path.join(logs, 'server.prev.log'));
  const log = fs.openSync(current, 'w');
  const lib = path.join(engine, 'lib');
  const surfaceManifest = path.join(engine, 'surface-manifest.json');
  const qualification = path.join(engine, 'artifact-qualification.ready');
  const env = { ...process.env };
  const nonce = randomBytes(32).toString('hex');
  env.ORCA_DESKTOP_READY_NONCE = nonce;
  delete env.ORCA_WEB_SOURCE_ARCHIVE;
  env.ORCA_WEB_SOURCE_URL = 'https://github.com/ZimengXiong/PancakeSlicer';
  delete env.ORCA_BUILD_REVISION;
  delete env.ORCA_BUILD_IMAGE;

  env.ORCA_WEB_FALLBACK_BACKEND = '';
  delete env.ORCA_WEB_SURFACE_MANIFEST;
  delete env.ORCA_WEB_ARTIFACT_MANIFEST;
  delete env.ORCA_WEB_ARTIFACT_QUALIFICATION;

  if (fs.existsSync(surfaceManifest)) env.ORCA_WEB_SURFACE_MANIFEST = surfaceManifest;
  if (fs.existsSync(qualification)) env.ORCA_WEB_ARTIFACT_QUALIFICATION = qualification;

  if (process.platform === 'linux' && fs.existsSync(lib)) env.LD_LIBRARY_PATH = [lib, env.LD_LIBRARY_PATH].filter(Boolean).join(':');
  let lastError = 'Oops! The local slicing service took too long to start up.';
  const child = spawn(path.join(engine, `orca-webapp-server${EXE}`), [
    '--listen', '127.0.0.1:29731',
    '--static', web,
    '--tool', path.join(engine, `nonplanar_slice_tool${EXE}`),
    '--resources', path.join(engine, 'resources'),
    '--data-dir', path.join(data, 'data'),
    '--job-root', path.join(data, 'jobs'),

    '--printer-network', 'lan',
  ], { env, stdio: ['ignore', log, log], windowsHide: true });
  fs.closeSync(log);
  server = child;
  let exited = false, ready = false;
  child.on('exit', (code, signal) => {
    exited = true;
    lastError = `Bundled service exited (${signal ?? code}); the fixed desktop origin may be occupied.`;
    if (ready && server === child && !quitting) fail('Oops! The local slicing service stopped unexpectedly.');
  });
  child.on('error', () => { exited = true; lastError = 'Oops! We couldn\'t start the local slicing service.'; });
  const deadline = Date.now() + 30000;
  while (!exited && Date.now() < deadline) {
    if (await healthy(nonce, expected) && !exited) { ready = true; return DESKTOP_ORIGIN; }
    await new Promise((r) => setTimeout(r, 100));
  }
  stopServer();
  throw new Error(lastError);
}

function stopServer(): Promise<void> {
  const child = server;
  server = null;
  if (!child || child.exitCode !== null || child.signalCode !== null) return Promise.resolve();
  const exited = new Promise<void>((resolve) => child.once('exit', () => resolve()));
  child.kill('SIGTERM');
  const timer = setTimeout(() => { if (child.exitCode === null) child.kill('SIGKILL'); }, 3000);
  return exited.finally(() => clearTimeout(timer));
}

function fail(message: string) {
  if (quitting) return;
  quitting = true;
  dialog.showErrorBox('PancakeSlicer', message);
  stopServer();
  app.exit(1);
}


function send(command: string) {
  if (command === 'open' || command === 'import' || command === 'export-gcode' || command === 'export-3mf') {

    void win?.webContents.executeJavaScript(
      `window.dispatchEvent(new CustomEvent('orca-desktop-menu', { detail: ${JSON.stringify(command)} }))`, true,
    ).catch(console.error);
    return;
  }
  win?.webContents.send('orca-desktop:menu', command);
}

function menu() {


  const item = (label: string, command: string, accelerator: string): MenuItemConstructorOptions =>
    ({ label, accelerator, click: () => send(command) });
  const template: MenuItemConstructorOptions[] = [
    ...(MAC ? [{ role: 'appMenu' } as MenuItemConstructorOptions] : []),
    {
      label: 'File',
      submenu: [
        item('New project', 'new', 'CmdOrCtrl+N'),
        item('Open project…', 'open', 'CmdOrCtrl+O'),
        { label: 'Projects', click: () => send('projects') },
        item('Import…', 'import', 'CmdOrCtrl+I'),
        { type: 'separator' },
        item('Save project', 'export-3mf', 'CmdOrCtrl+S'),
        item('Export G-code', 'export-gcode', 'CmdOrCtrl+G'),
        { type: 'separator' },
        MAC ? { role: 'close' } : { role: 'quit' },
      ],
    },
    {
      label: 'Edit',
      submenu: [
        item('Undo', 'undo', 'CmdOrCtrl+Z'),
        item('Redo', 'redo', MAC ? 'Shift+Cmd+Z' : 'Ctrl+Y'),
        { type: 'separator' },
        { role: 'cut' }, { role: 'copy' }, { role: 'paste' }, { role: 'selectAll' },
      ],
    },
    {
      label: 'View',
      submenu: [
        { role: 'resetZoom' }, { role: 'zoomIn' }, { role: 'zoomOut' }, { type: 'separator' },
        { role: 'togglefullscreen' }, { role: 'toggleDevTools' },
      ],
    },
    { role: 'windowMenu' },
    {
      role: 'help',
      submenu: [
        { label: 'Report a problem…', click: () => void shell.openExternal('https://github.com/ZimengXiong/PancakeSlicer/issues/new?body=' +
          encodeURIComponent(`PancakeSlicer v${app.getVersion()}, ${process.platform} ${process.arch}\n\nAttach server.log from Help > Open logs folder.`)) },
        { label: 'Open logs folder', click: () => void shell.openPath(path.join(app.getPath('userData'), 'logs')) },
      ],
    },
  ];
  Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}

function storage() {
  const store = new ProjectStore(app.getPath('userData'));
  const legacy = new LegacyWorkspaceSource(app.getPath('userData'));
  projects = store;
  let legacyReady: Promise<void> | null = null;
  let exporting = false;
  const pendingWrites = new Set<Promise<unknown>>();
  const readers = new Set(['list', 'load', 'getBlob', 'hasBlob']);
  const ops = ['list', 'load', 'save', 'rename', 'duplicate', 'remove', 'restore', 'putBlob', 'getBlob', 'hasBlob', 'emptyTrash'] as const;
  for (const op of ops) {
    ipcMain.handle(`orca-desktop:projects:${op}`, async (e, ...args: unknown[]) => {
      if (!origin || new URL(e.senderFrame?.url ?? 'about:blank').origin !== origin) throw new Error('not the app');
      if (exporting && !readers.has(op)) throw new Error('Hang tight—saving to the old workspace is paused while we migrate your files. Try migrating again (your original files are safe!).');

      const operation = (legacyReady ??= store.init()).then(() => (store[op] as (...a: unknown[]) => Promise<unknown>).apply(store, args));
      if (!readers.has(op)) pendingWrites.add(operation);
      try { return await operation; }
      catch (error) {

        if (error instanceof Error && ['ProjectConflictError', 'ProjectDeletedError'].includes(error.name))
          return { __orcaProjectError: { name: error.name, message: error.message } };
        throw error;
      } finally { pendingWrites.delete(operation); }
    });
  }
  ipcMain.handle('orca-desktop:legacy-projects:inventory', async (e) => {
    if (!origin || new URL(e.senderFrame?.url ?? 'about:blank').origin !== origin) throw new Error('not the app');


    exporting = true;
    await Promise.all([...pendingWrites]);
    await store.flush();
    return legacy.inventory();
  });
  ipcMain.handle('orca-desktop:legacy-projects:read-file', (e, request: LegacyRead) => {
    if (!origin || new URL(e.senderFrame?.url ?? 'about:blank').origin !== origin) throw new Error('not the app');
    return legacy.readFile(request);
  });
  ipcMain.handle('orca-desktop:legacy-renderer:inventory', (e) => {
    if (!origin || new URL(e.senderFrame?.url ?? 'about:blank').origin !== origin) throw new Error('not the app');
    return legacyRenderer.inventory();
  });
  ipcMain.handle('orca-desktop:legacy-renderer:read-part', (e, request: LegacyRendererPartRead) => {
    if (!origin || new URL(e.senderFrame?.url ?? 'about:blank').origin !== origin) throw new Error('not the app');
    return legacyRenderer.readPart(request);
  });
  ipcMain.handle('orca-desktop:bambu-connect', (e, jobId: unknown, filename: unknown) => {
    if (!origin || new URL(e.senderFrame?.url ?? 'about:blank').origin !== origin) throw new Error('not the app');
    return openInBambuConnect(jobId, filename);
  });
}



async function openInBambuConnect(jobId: unknown, filename: unknown) {
  if (typeof jobId !== 'string' || !/^[0-9a-f]{16,64}$/.test(jobId)) throw new Error('Oops! That slice job isn\'t valid.');
  if (typeof filename !== 'string' || !/\.gcode\.3mf$/i.test(filename) || /[\\/\u0000-\u001f]/.test(filename) || filename.length > 240) throw new Error('Oops! That file name isn\'t valid.');
  const dir = path.join(app.getPath('temp'), 'orca-bambu-connect');
  fs.mkdirSync(dir, { recursive: true });
  const target = path.join(dir, filename);
  await new Promise<void>((resolve, reject) => {
    http.get(`${DESKTOP_ORIGIN}/api/jobs/${jobId}/download`, (res) => {
      if (res.statusCode !== 200) { res.resume(); reject(new Error(`Oops! The sliced file isn't available anymore (HTTP ${res.statusCode}). Please slice the plate again.`)); return; }
      const out = fs.createWriteStream(target);
      res.pipe(out);
      out.on('finish', () => resolve());
      out.on('error', reject);
    }).on('error', reject);
  });
  const name = filename.replace(/\.gcode\.3mf$/i, '');
  const url = `bambu-connect://import-file?path=${encodeURIComponent(process.platform === 'win32' ? target.replace(/\\/g, '/') : target)}&name=${encodeURIComponent(name)}&version=1.0.0`;
  try { await shell.openExternal(url); }
  catch { throw new Error('Bambu Connect isn\'t installed on this computer yet. Download it from bambulab.com, then try sending again.'); }
}


function restrictNetwork(target: Electron.Session) {
  target.setSpellCheckerEnabled(false);
  target.webRequest.onBeforeRequest({ urls: ['<all_urls>'] }, (details, callback) => {
    const url = new URL(details.url);
    const local = url.origin === DESKTOP_ORIGIN;
    const update = url.protocol === 'https:' && url.hostname === 'api.github.com'
      && url.pathname === '/repos/ZimengXiong/PancakeSlicer/releases' && url.search === '?per_page=30';
    const embedded = ['blob:', 'data:', 'devtools:'].includes(url.protocol);
    const allow = local || update || embedded;
    if (!allow) console.error(`Blocked outbound request: ${url.origin}${url.pathname}`);
    callback({ cancel: !allow });
  });
}

async function start() {
  restrictNetwork(session.defaultSession);
  menu();
  const serving = Promise.all([startServer(), storage()]);
  if (app.commandLine.hasSwitch('service-only')) { [origin] = await serving; return; }
  const rendererSession = session.fromPartition('orca-service-renderer', { cache: false });
  restrictNetwork(rendererSession);
  if (rendererSession.isPersistent() || rendererSession.getStoragePath() !== null) throw new Error('Oops! The app window needs an isolated in-memory session to run safely.');
  win = new BrowserWindow({
    width: 1440,
    height: 900,
    minWidth: 800,
    minHeight: 600,
    show: false,
    title: 'PancakeSlicer',
    backgroundColor: '#363636',
    webPreferences: { session: rendererSession, preload: path.join(__dirname, 'preload.js'), contextIsolation: true, sandbox: true, nodeIntegration: false, spellcheck: false },
  });
  win.once('ready-to-show', () => win?.show());
  win.on('close', (e) => {
    if (quitReady) return;
    e.preventDefault();
    if (MAC && !closing) win?.hide();
    else void prepareQuit();
  });
  win.on('closed', () => { win = null; });
  const contents = win.webContents;
  contents.on('console-message', (details) => {
    if (details.level === 'warning' || details.level === 'error')
      console.error(`Renderer ${details.level}: ${details.message} (${details.sourceId}:${details.lineNumber})`);
  });

  contents.on('will-prevent-unload', (e) => e.preventDefault());
  contents.on('will-navigate', (e, target) => {
    if (new URL(target).origin !== origin) { e.preventDefault(); shell.openExternal(target); }
  });
  contents.setWindowOpenHandler(({ url: target }) => {
    const url = new URL(target);
    if (url.origin === origin && (url.pathname.startsWith('/help/') || ['/licence.txt', '/third-party-licences.txt'].includes(url.pathname))) {
      const title = url.pathname.startsWith('/help/') ? 'PancakeSlicer user guide' : 'PancakeSlicer licences';
      const guide = new BrowserWindow({ width: 1000, height: 800, title,
        webPreferences: { session: rendererSession, contextIsolation: true, sandbox: true, nodeIntegration: false, spellcheck: false } });
      guide.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
      if (!url.pathname.startsWith('/help/')) guide.on('page-title-updated', (event) => event.preventDefault());
      guide.webContents.on('will-navigate', (event, next) => { if (new URL(next).origin !== origin) event.preventDefault(); });
      void guide.loadURL(target).catch(console.error);
    } else if (url.protocol === 'https:' && url.origin !== origin) void shell.openExternal(target);
    return { action: 'deny' };
  });
  await win.loadURL(`data:text/html,<html style="background:%23363636"></html>`);
  [origin] = await serving;
  if (win && !closing) await win.loadURL(origin);
}

app.on('window-all-closed', () => app.quit());
app.on('activate', () => { if (win && !closing) { win.show(); win.focus(); } });
app.on('before-quit', (e) => {
  if (quitReady) { quitting = true; return; }
  e.preventDefault();
  void prepareQuit();
});
app.on('will-quit', () => void stopServer());
process.on('exit', () => void stopServer());
process.on('SIGTERM', () => app.quit());
process.on('SIGINT', () => app.quit());


let closingSince = 0;

async function prepareQuit() {
  if (closing) {
    if (Date.now() - closingSince > 2000) { await stopServer(); app.exit(0); }
    return;
  }
  closing = true;
  closingSince = Date.now();
  quitting = true;
  win?.setEnabled(false);
  let timer: ReturnType<typeof setTimeout> | undefined;
  try {
    const flush = async () => {
      const contents = win?.webContents;
      if (origin && contents && !contents.isDestroyed() && !contents.isCrashed() && contents.getURL().startsWith(origin))
        await contents.executeJavaScript('window.orcaDesktop?.flushBeforeClose?.()');
      await projects?.flush();
    };
    const finished = await Promise.race([
      flush().then(() => true),
      new Promise<false>((resolve) => { timer = setTimeout(() => resolve(false), 10000); }),
    ]);


    if (!finished) throw new Error('Still saving your work! We\'ll keep the window open for a moment—try closing it again once saving finishes.');
    win?.hide();
    await stopServer();
    quitReady = true;
    app.quit();
  } catch (e) {
    quitting = false;
    closing = false;
    win?.setEnabled(true);
    dialog.showErrorBox('PancakeSlicer', e instanceof Error ? e.message : String(e));
  } finally {
    if (timer) clearTimeout(timer);
  }
}
