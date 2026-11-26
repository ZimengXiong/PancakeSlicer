
import { app, BrowserWindow, clipboard, dialog, ipcMain, Menu, session, shell, type MenuItemConstructorOptions } from 'electron';
import { execFileSync, spawn, type ChildProcess } from 'node:child_process';
import { createHash, createHmac, randomBytes, timingSafeEqual } from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { agentClients, configureAgentClient, formatMcpConfig, mcpLaunch, publishConnection, readAgentSettings, removeConnection, writeAgentSettings } from './agents';
import { fileURLToPath } from 'node:url';
import { isSliceTimeout, MAX_SLICE_TIMEOUT_S, MIN_SLICE_TIMEOUT_S } from './slice-timeout.js';

const MAC = process.platform === 'darwin';
const WINDOWS = process.platform === 'win32';
const EXE = WINDOWS ? '.exe' : '';
const PREFERRED_PORT = 29731;
const SERVER_GUARD = 'pancakeslicer-server-guard.exe';
const ACCESS_TOKEN = randomBytes(32).toString('hex');
const LAUNCH_MODEL = /\.(stl|obj|3mf|step|stp)$/i;
const MAX_LAUNCH_MODEL_BYTES = 256 * 1024 * 1024;
const launchModels = new Map<number, Map<number, string>>();
let launchId = 0;
const OPERATING_SYSTEM = `${process.platform} ${os.release()} ${process.arch}`;
const DIAGNOSTIC_LOG_LINES = 200;

let server: ChildProcess | null = null;
let stoppingServer: Promise<void> | undefined;
let origin = '';
let win: BrowserWindow | null = null;
let quitting = false;
let quitReady = false;
let closing = false;
let deviceUi: { origin: string; apiOrigin: string; sockets: string[] } | null = null;

const STARTING_PAGE = '<!doctype html><title>PancakeSlicer</title><body style="margin:0;height:100vh;display:flex;'
  + 'align-items:center;justify-content:center;background:#363636;color:#d0d0d0;font:15px system-ui,sans-serif;'
  + 'user-select:none;cursor:default">PancakeSlicer is starting…</body>';



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
  queueLaunchArguments(process.argv, process.cwd());
  app.on('second-instance', (_e, argv, cwd) => {
    queueLaunchArguments(argv, cwd);
    if (!win) return;
    if (win.isMinimized()) win.restore();
    win.show();
    win.focus();
  });
  if (MAC) app.on('open-file', (e, file) => {
    e.preventDefault();
    queueLaunchArguments([file], process.cwd());
  });
  app.whenReady().then(start).catch((e) => fail(String(e?.stack ?? e)));
}

function queueLaunchArguments(argv: readonly string[], cwd: string) {
  const paths: string[] = [];
  for (let arg of argv) {
    if (/^file:/i.test(arg)) {
      try { arg = fileURLToPath(arg); } catch { continue; }
    }
    if (LAUNCH_MODEL.test(arg)) paths.push(path.resolve(cwd, arg));
  }
  if (!paths.length) return;
  launchModels.set(++launchId, new Map(paths.entries()));
  if (win && origin && !win.webContents.isDestroyed()) win.webContents.send('orca-desktop:launch-files-available');
}

async function readLaunchModels(after: number) {
  const batch = [...launchModels].find(([id]) => id > after);
  if (!batch) return null;
  const [id, paths] = batch;
  const files: { index: number; name: string; data: Buffer; lastModified: number }[] = [];
  const errors: string[] = [];
  let total = 0;
  for (const [index, file] of paths) {
    try {
      const handle = await fs.promises.open(file, 'r');
      try {
        const stat = await handle.stat();
        if (!stat.isFile()) throw new Error('not a regular file');
        if (stat.size > MAX_LAUNCH_MODEL_BYTES - total)
          throw new Error(`model files must not exceed ${MAX_LAUNCH_MODEL_BYTES / 1024 / 1024} MiB per batch`);
        const changed = () => new Error('the file changed while it was being read');
        const data = Buffer.alloc(stat.size);
        for (let offset = 0; offset < data.length;) {
          const { bytesRead } = await handle.read(data, offset, data.length - offset, offset);
          if (!bytesRead) throw changed();
          offset += bytesRead;
        }
        if ((await handle.stat()).size !== stat.size) throw changed();
        total += data.byteLength;
        files.push({ index, name: path.basename(file), data, lastModified: stat.mtimeMs });
      } finally { await handle.close(); }
    } catch (e) { errors.push(`${file}: ${e instanceof Error ? e.message : String(e)}`); }
  }
  if (errors.length) dialog.showErrorBox('PancakeSlicer', `Oops! Some model files couldn\'t be opened.\n${errors.join('\n')}`);
  return { id, files };
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
  server_guard_sha256?: string;
  native_manifest_sha256: string;
  native_qualification_sha256: string;
  support_fins_sha256: string;

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
    ['support-fins/support-fins.mjs', 'support_fins_sha256'],
  ] as const).map(async ([name, key]) => /^[a-f0-9]{64}$/.test(expected[key]) && await fileHash(path.join(engine, name)) === expected[key]));
  if (WINDOWS) hashes.push(typeof expected.server_guard_sha256 === 'string' && /^[a-f0-9]{64}$/.test(expected.server_guard_sha256) && await fileHash(path.join(engine, SERVER_GUARD)) === expected.server_guard_sha256);
  if (!hashes.every(Boolean)) throw new Error('Oops! One of the app\'s bundled files looks modified or corrupted. Try reinstalling PancakeSlicer.');
  if (!/^[a-f0-9]{64}$/.test(expected.native_resources_sha256)) throw new Error('Oops! Required app resources are missing from this installation. Try reinstalling PancakeSlicer.');
  const schemas = JSON.parse(fs.readFileSync(path.join(engine, 'schemas.json'), 'utf8')) as Record<string, number>;
  if (!Object.keys(schemas).length || !Object.values(schemas).every((v) => Number.isInteger(v) && v > 0)) throw new Error('Oops! Bundled server configuration files are missing. Try reinstalling PancakeSlicer.');
  return { ...expected, schemas };
}

function healthy(serverOrigin: string, nonce: string, expected: BundleIdentity): Promise<boolean> {
  return new Promise((resolve) => {
    const req = http.get(`${serverOrigin}/api/healthz`, { timeout: 1000, headers: { 'Cache-Control': 'no-store' } }, (res) => {
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

function systemLine() {
  const system = `${OPERATING_SYSTEM}, locale ${app.getSystemLocale()}`;
  if (process.platform === 'win32') {
    try {
      const output = execFileSync('reg', ['query', 'HKLM\\SYSTEM\\CurrentControlSet\\Control\\Nls\\CodePage', '/v', 'ACP'], {
        encoding: 'utf8', windowsHide: true,
      });
      return `${system}, ANSI code page ${/ACP\s+REG_SZ\s+(\d+)/.exec(output)?.[1] ?? '?'}`;
    } catch {}
  }
  return system;
}

function diagnostics() {
  const logs = path.join(app.getPath('userData'), 'logs');
  const tail = (name: string) => {
    try {
      const log = fs.readFileSync(path.join(logs, name), 'utf8');
      return log.trimEnd().split('\n').slice(-DIAGNOSTIC_LOG_LINES).join('\n');
    } catch { return '(missing)'; }
  };
  const text = `PancakeSlicer v${app.getVersion()}\n${systemLine()}\n\n`
    + `--- server.log (last ${DIAGNOSTIC_LOG_LINES} lines) ---\n${tail('server.log')}\n\n`
    + `--- server.prev.log (last ${DIAGNOSTIC_LOG_LINES} lines) ---\n${tail('server.prev.log')}`;
  return '```\n' + text.split(os.homedir()).join('~') + '\n```';
}

function reportProblem() {
  const query = new URLSearchParams({ template: 'bug.yml', version: `v${app.getVersion()}`, os: OPERATING_SYSTEM });
  void shell.openExternal(`https://github.com/ZimengXiong/PancakeSlicer/issues/new?${query}`);
}

function probePort(port: number): Promise<number> {
  return new Promise((resolve, reject) => {
    const probe = net.createServer((socket) => socket.destroy());
    probe.once('error', reject);
    probe.listen(port, '127.0.0.1', () => {
      const address = probe.address() as net.AddressInfo;
      probe.close((error) => error ? reject(error) : resolve(address.port));
    });
  });
}

async function startServer(): Promise<string> {
  const { web, engine } = payload();
  const expected = await bundleIdentity(web, engine);
  const port = await probePort(PREFERRED_PORT).catch((error: NodeJS.ErrnoException) => {
    if (error.code !== 'EADDRINUSE' && error.code !== 'EACCES') throw error;
    return probePort(0);
  });
  if (quitting) throw new Error('The app is closing.');
  const listen = `127.0.0.1:${port}`;
  const serverOrigin = `http://${listen}`;
  const data = app.getPath('userData');
  const logs = path.join(data, 'logs');
  fs.mkdirSync(logs, { recursive: true });
  const current = path.join(logs, 'server.log');
  const details = `Details are in ${current}`;
  // The previous launch's log stays as server.prev.log, so a crash can still be reported after a restart.
  if (fs.existsSync(current)) fs.renameSync(current, path.join(logs, 'server.prev.log'));
  const log = fs.openSync(current, 'w');
  const lib = path.join(engine, 'lib');
  const surfaceManifest = path.join(engine, 'surface-manifest.json');
  const qualification = path.join(engine, 'artifact-qualification.ready');
  const env = { ...process.env };
  const nonce = randomBytes(32).toString('hex');
  env.ORCA_DESKTOP_READY_NONCE = nonce;
  env.ORCA_WEB_BEARER_TOKEN = ACCESS_TOKEN;
  delete env.ORCA_WEB_SOURCE_ARCHIVE;
  env.ORCA_WEB_SOURCE_URL = 'https://github.com/ZimengXiong/PancakeSlicer';
  delete env.ORCA_BUILD_REVISION;
  delete env.ORCA_BUILD_IMAGE;

  delete env.ORCA_WEB_SURFACE_MANIFEST;
  delete env.ORCA_WEB_ARTIFACT_MANIFEST;
  delete env.ORCA_WEB_ARTIFACT_QUALIFICATION;

  if (fs.existsSync(surfaceManifest)) env.ORCA_WEB_SURFACE_MANIFEST = surfaceManifest;
  if (fs.existsSync(qualification)) env.ORCA_WEB_ARTIFACT_QUALIFICATION = qualification;
  env.ORCA_WEB_SUPPORT_FINS_NODE = process.execPath;
  env.ORCA_WEB_SUPPORT_FINS_CLI = path.join(engine, 'support-fins', 'support-fins.mjs');
  env.ORCA_WEB_SUPPORT_FINS_ELECTRON = '1';

  if (process.platform === 'linux' && fs.existsSync(lib)) env.LD_LIBRARY_PATH = [lib, env.LD_LIBRARY_PATH].filter(Boolean).join(':');
  let lastError = '';
  const executable = path.join(engine, `orca-webapp-server${EXE}`);
  const args = [
    '--listen', listen,
    '--static', web,
    '--tool', path.join(engine, `nonplanar_slice_tool${EXE}`),
    '--resources', path.join(engine, 'resources'),
    '--data-dir', path.join(data, 'data'),
    '--job-root', path.join(data, 'jobs'),
    '--job-timeout', '0',

    '--printer-network', 'lan',
  ];
  const child = spawn(WINDOWS ? path.join(engine, SERVER_GUARD) : executable, WINDOWS ? [executable, ...args] : args,
    { env, stdio: [WINDOWS ? 'pipe' : 'ignore', log, log], windowsHide: true });
  fs.closeSync(log);
  server = child;
  child.stdin?.on('error', () => {});
  let exited = false, ready = false;
  child.on('exit', (code, signal) => {
    exited = true;
    lastError ||= `Bundled service exited (${signal ?? code}).\n${details}`;
    if (ready && server === child && !quitting) void fail(`Oops! The local slicing service stopped unexpectedly.\n${details}`);
  });
  child.on('error', () => {
    if (child.pid !== undefined) return;
    exited = true;
    lastError = `Oops! We couldn't start the local slicing service.\n${details}`;
  });
  // A cold start on a slow PC with on-access virus scanning can take minutes, so wait for as long as the service lives.
  while (!exited) {
    if (await healthy(serverOrigin, nonce, expected) && !exited && !quitting) { ready = true; return serverOrigin; }
    await new Promise((r) => setTimeout(r, 100));
  }
  await stopServer();
  throw new Error(lastError);
}

function stopServer(): Promise<void> {
  if (stoppingServer) return stoppingServer;
  const child = server;
  server = null;
  if (!child?.pid || child.exitCode !== null || child.signalCode !== null) return Promise.resolve();
  const exited = new Promise<void>((resolve) => child.once('exit', () => resolve()));
  if (WINDOWS) child.stdin?.end();
  else child.kill('SIGTERM');
  const timer = setTimeout(() => child.kill('SIGKILL'), WINDOWS ? 6000 : 3000);
  stoppingServer = exited.finally(() => clearTimeout(timer));
  return stoppingServer;
}

async function fail(message: string) {
  if (quitting) return;
  quitting = true;
  await stopServer();
  dialog.showErrorBox('PancakeSlicer', message);
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
        { label: 'Report a problem…', click: reportProblem },
        { label: 'Copy diagnostics', click: () => clipboard.writeText(diagnostics()) },
        { label: 'Open logs folder', click: () => void shell.openPath(path.join(app.getPath('userData'), 'logs')) },
      ],
    },
  ];
  Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}

const agentLaunch = () => mcpLaunch(process.execPath,
  app.isPackaged ? path.join(process.resourcesPath, 'app.asar.unpacked', 'out', 'mcp.cjs') : path.join(__dirname, 'mcp.cjs'), app.getPath('userData'));
let serviceUrl = '';
const CONTROL_TOKEN = randomBytes(32).toString('hex');
const AGENT_MODEL = /\.(stl|obj|3mf|step|stp)$/i;
let controlUrl = '';
let commandId = 0;
const windowCommands = new Map<number, { resolve: (value: unknown) => void; reject: (error: Error) => void; timer: NodeJS.Timeout }>();

function syncAgentConnection() {
  const data = app.getPath('userData');
  try {
    publishConnection(data, readAgentSettings(data), serviceUrl, ACCESS_TOKEN, controlUrl ? { url: controlUrl, token: CONTROL_TOKEN } : undefined);
  } catch (error) {
    console.error('AI agent access could not be updated.', error);
  }
}

function windowCommand(command: string, args: Record<string, unknown>, timeoutMs: number): Promise<unknown> {
  const target = win;
  if (!target || !origin || target.webContents.isDestroyed()) return Promise.reject(new Error("The PancakeSlicer window isn't open."));
  if (target.isMinimized()) target.restore();
  const id = ++commandId;
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => { windowCommands.delete(id); reject(new Error(`PancakeSlicer didn't finish "${command}" in time.`)); }, timeoutMs);
    windowCommands.set(id, { resolve, reject, timer });
    target.webContents.send('orca-desktop:agent-command', id, command, args);
  });
}

const refused = (status: number, message: string) => Object.assign(new Error(message), { status });

async function controlRequest(body: { command?: unknown; args?: unknown }) {
  const settings = readAgentSettings(app.getPath('userData'));
  if (!settings.enabled) throw refused(403, 'AI agent access is off in Preferences › AI agents.');
  const command = typeof body.command === 'string' ? body.command : '';
  const args: Record<string, unknown> = body.args && typeof body.args === 'object' ? { ...(body.args as Record<string, unknown>) } : {};
  if (command === 'open_model') {
    const file = typeof args.path === 'string' ? args.path : '';
    if (!path.isAbsolute(file) || !AGENT_MODEL.test(file)) throw refused(400, 'open_model needs an absolute path to an STL, OBJ, 3MF or STEP file.');
    const stat = await fs.promises.stat(file);
    if (!stat.isFile() || stat.size > MAX_LAUNCH_MODEL_BYTES) throw refused(400, 'That model is not a file or is too large to open.');
    args.name = path.basename(file);
    args.data = await fs.promises.readFile(file);
    delete args.path;
  }
  if (command !== 'slice') return windowCommand(command, args, 120_000);
  const requested = Number(args.timeout_ms) / 1000;
  const timeoutMs = (isSliceTimeout(requested) ? requested : settings.sliceTimeoutS) * 1000;
  return windowCommand(command, { ...args, timeout_ms: timeoutMs }, timeoutMs + 15_000);
}

function startControl(): Promise<void> {
  const control = http.createServer((req, res) => {
    const send = (status: number, value: unknown) => {
      res.writeHead(status, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
      res.end(JSON.stringify(value));
    };
    if (req.method !== 'POST' || req.url !== '/command') return send(404, { error: 'Not found.' });
    const auth = Buffer.from(req.headers.authorization ?? ''), want = Buffer.from(`Bearer ${CONTROL_TOKEN}`);
    if (auth.length !== want.length || !timingSafeEqual(auth, want)) return send(401, { error: 'Not authorised.' });
    let raw = '';
    req.setEncoding('utf8');
    req.on('data', (chunk: string) => { raw += chunk; if (raw.length > 1 << 20) req.destroy(); });
    req.on('end', () => {
      let body: { command?: unknown; args?: unknown };
      try { body = JSON.parse(raw); } catch { return send(400, { error: 'Expected JSON.' }); }
      controlRequest(body).then((result) => send(200, { result }), (error: Error & { status?: number }) => send(error.status ?? 409, { error: error.message }));
    });
  });
  return new Promise((resolve, reject) => {
    control.once('error', reject);
    control.listen(0, '127.0.0.1', () => {
      const address = control.address();
      if (address && typeof address === 'object') controlUrl = `http://127.0.0.1:${address.port}`;
      resolve();
    });
  });
}

async function agentState() {
  const launch = agentLaunch();
  const { customConfigs, ...settings } = readAgentSettings(app.getPath('userData'));
  return { ...settings, minSliceTimeoutS: MIN_SLICE_TIMEOUT_S, maxSliceTimeoutS: MAX_SLICE_TIMEOUT_S, clients: await agentClients(launch, customConfigs), launch };
}

function ipc() {
  const fromApp = (e: Electron.IpcMainInvokeEvent) => {
    if (!origin || e.sender !== win?.webContents || e.senderFrame !== e.sender.mainFrame || e.senderFrame.origin !== origin)
      throw new Error('not the app');
  };
  ipcMain.handle('orca-desktop:agents', (e) => {
    fromApp(e);
    return agentState();
  });
  ipcMain.handle('orca-desktop:agents-copy-config', (e, format: unknown) => {
    fromApp(e);
    clipboard.writeText(formatMcpConfig(agentLaunch(), format));
  });
  ipcMain.handle('orca-desktop:agents-set', (e, enabled: unknown, printers: unknown) => {
    fromApp(e);
    const data = app.getPath('userData');
    const saved = writeAgentSettings(data, { ...readAgentSettings(data), enabled: enabled === true, printers: printers === true });
    syncAgentConnection();
    return { enabled: saved.enabled, printers: saved.printers };
  });
  ipcMain.handle('orca-desktop:agents-slice-timeout', (e, seconds: unknown) => {
    fromApp(e);
    if (!isSliceTimeout(seconds)) throw new Error(`Enter a whole number from ${MIN_SLICE_TIMEOUT_S} to ${MAX_SLICE_TIMEOUT_S} seconds.`);
    const data = app.getPath('userData');
    writeAgentSettings(data, { ...readAgentSettings(data), sliceTimeoutS: seconds });
    syncAgentConnection();
    return { sliceTimeoutS: seconds };
  });
  ipcMain.handle('orca-desktop:agent-reply', (e, id: unknown, ok: unknown, result: unknown) => {
    fromApp(e);
    const pending = typeof id === 'number' ? windowCommands.get(id) : undefined;
    if (!pending) return;
    windowCommands.delete(id as number);
    clearTimeout(pending.timer);
    if (ok === true) pending.resolve(result);
    else pending.reject(new Error(String(result)));
  });
  ipcMain.handle('orca-desktop:agents-client', async (e, id: unknown, add: unknown) => {
    fromApp(e);
    if (typeof id !== 'string') throw new Error('Unknown AI client.');
    const { customConfigs } = readAgentSettings(app.getPath('userData'));
    await configureAgentClient(id, add === true ? agentLaunch() : null, customConfigs);
    return agentState();
  });
  ipcMain.handle('orca-desktop:bambu-connect', (e, jobId: unknown, filename: unknown) => {
    if (!origin || new URL(e.senderFrame?.url ?? 'about:blank').origin !== origin) throw new Error('not the app');
    return openInBambuConnect(jobId, filename);
  });
  ipcMain.handle('orca-desktop:read-launch-files', (e, after: number) => {
    if (!origin || new URL(e.senderFrame?.url ?? 'about:blank').origin !== origin) throw new Error('not the app');
    return readLaunchModels(after);
  });
  ipcMain.handle('orca-desktop:accept-launch-file', (e, id: number, index: number) => {
    if (!origin || new URL(e.senderFrame?.url ?? 'about:blank').origin !== origin) throw new Error('not the app');
    const files = launchModels.get(id);
    if (files?.delete(index) && !files.size) launchModels.delete(id);
  });
  ipcMain.handle('orca-desktop:device-ui', (e, address: unknown, apiAddress: unknown) => {
    if (!origin || e.sender !== win?.webContents || e.senderFrame !== e.sender.mainFrame || e.senderFrame.origin !== origin)
      throw new Error('not the app');
    deviceUi = null;
    if (address === null) return;
    const ui = deviceHttpUrl(address);
    const api = apiAddress === undefined ? ui : deviceHttpUrl(apiAddress);
    if (!ui || !api) throw new Error('Invalid device web UI address');
    const socket = (url: URL) => `${url.protocol === 'https:' ? 'wss:' : 'ws:'}//${url.host}`;
    deviceUi = { origin: ui.origin, apiOrigin: api.origin, sockets: [...new Set([socket(ui), socket(api)])] };
  });
}



async function openInBambuConnect(jobId: unknown, filename: unknown) {
  if (typeof jobId !== 'string' || !/^[0-9a-f]{16,64}$/.test(jobId)) throw new Error('Oops! That slice job isn\'t valid.');
  if (typeof filename !== 'string' || !/\.gcode\.3mf$/i.test(filename) || /[\\/\u0000-\u001f]/.test(filename) || filename.length > 240) throw new Error('Oops! That file name isn\'t valid.');
  const dir = path.join(app.getPath('temp'), 'orca-bambu-connect');
  fs.mkdirSync(dir, { recursive: true });
  const target = path.join(dir, filename);
  await new Promise<void>((resolve, reject) => {
    http.get(`${origin}/api/jobs/${jobId}/download`, (res) => {
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


function deviceHttpUrl(address: unknown) {
  const url = typeof address === 'string' ? URL.parse(address) : null;
  if (!url || !['http:', 'https:'].includes(url.protocol) || url.username || url.password || url.port === URL.parse(origin)?.port) return null;
  const host = url.hostname.toLowerCase().replace(/^\[|\]$/g, '');
  if (host === 'localhost' || host.endsWith('.localhost') || host === '::1' || host === '127.0.0.1' || host.startsWith('127.')) return null;
  // Origins are interpolated into CSP source expressions.
  if (!/^(?:[a-z0-9.-]+|\[[0-9a-f:]+\])$/.test(url.hostname)) return null;
  return url;
}

function hasParentFrame(details: Electron.OnBeforeRequestListenerDetails): boolean {
  try { return !!details.frame?.parent; } catch { return true; }
}

function restrictNetwork(target: Electron.Session, appContents?: Electron.WebContents) {
  target.setSpellCheckerEnabled(false);
  target.webRequest.onBeforeRequest((details, callback) => {
    const url = new URL(details.url);
    const { initiatorOrigin, resourceType, webContentsId } = details;
    const selected = appContents && webContentsId === appContents.id ? deviceUi : null;
    const fromPrinter = !!selected && initiatorOrigin === selected.origin;
    // Navigation requests are attributed to their initiator, including redirects.
    if (resourceType === 'subFrame') {
      callback({ cancel: !selected || deviceHttpUrl(details.url)?.origin !== selected.origin
        || (initiatorOrigin !== origin && !fromPrinter) });
      return;
    }
    if (fromPrinter) {
      const http = deviceHttpUrl(details.url);
      const allow = resourceType !== 'mainFrame' && (http?.origin === selected.origin
        || (resourceType === 'xhr' && http?.origin === selected.apiOrigin)
        || (resourceType === 'webSocket' && selected.sockets.includes(url.origin))
        || url.protocol === 'data:' || (url.protocol === 'blob:' && url.origin === selected.origin));
      callback({ cancel: !allow });
      return;
    }
    const local = url.origin === origin;
    const update = url.protocol === 'https:' && url.hostname === 'api.github.com'
      && url.pathname === '/repos/ZimengXiong/PancakeSlicer/releases' && url.search === '?per_page=30';
    const embedded = ['blob:', 'data:', 'devtools:'].includes(url.protocol);
    const allow = (initiatorOrigin === undefined || initiatorOrigin === origin)
      && !hasParentFrame(details) && (local || update || embedded);
    if (!allow) console.error(`Blocked outbound request: ${url.origin}${url.pathname}`);
    callback({ cancel: !allow });
  });
  if (!appContents) return;
  target.webRequest.onHeadersReceived((details, callback) => {
    const selected = deviceUi;
    if (!selected || details.resourceType !== 'subFrame' || details.webContentsId !== appContents.id
      || deviceHttpUrl(details.url)?.origin !== selected.origin) {
      callback({});
      return;
    }
    const headers = { ...details.responseHeaders };
    const key = Object.keys(headers).find((name) => name.toLowerCase() === 'content-security-policy') ?? 'Content-Security-Policy';
    const endpoints = [...new Set([selected.origin, selected.apiOrigin, ...selected.sockets])];
    headers[key] = [...(headers[key] ?? []), `connect-src ${endpoints.join(' ')}; frame-src 'self'; form-action 'self'; object-src 'none'; frame-ancestors ${origin} 'self'`];
    callback({ responseHeaders: headers });
  });
  target.setPermissionCheckHandler((_contents, _permission, requestingOrigin, details) =>
    details.isMainFrame && URL.parse(requestingOrigin)?.origin === origin);
  target.setPermissionRequestHandler((_contents, _permission, callback, details) =>
    callback(details.isMainFrame && URL.parse(details.requestingUrl)?.origin === origin));
}

async function start() {
  restrictNetwork(session.defaultSession);
  menu();
  ipc();
  const serving = startServer();
  void serving.catch(() => {});
  process.once('exit', () => removeConnection(app.getPath('userData')));
  const controlling = app.commandLine.hasSwitch('service-only') ? Promise.resolve()
    : startControl().catch((error) => console.error('Agent control could not start.', error));
  void Promise.all([serving, controlling]).then(([url]) => {
    serviceUrl = url;
    syncAgentConnection();
  }, () => undefined);
  if (app.commandLine.hasSwitch('service-only')) { origin = await serving; return; }
  const rendererSession = session.fromPartition('orca-service-renderer', { cache: false });
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
  restrictNetwork(rendererSession, contents);
  contents.on('console-message', (details) => {
    if (details.level === 'warning' || details.level === 'error')
      console.error(`Renderer ${details.level}: ${details.message} (${details.sourceId}:${details.lineNumber})`);
  });

  contents.on('will-prevent-unload', (e) => e.preventDefault());
  const restrictFrame = (event: Electron.Event<Electron.WebContentsWillFrameNavigateEventParams>) => {
    if (!event.isMainFrame && (!deviceUi || deviceHttpUrl(event.url)?.origin !== deviceUi.origin)) event.preventDefault();
  };
  contents.on('will-frame-navigate', restrictFrame);
  contents.on('will-redirect', (event) => {
    if (event.isMainFrame && new URL(event.url).origin !== origin) event.preventDefault();
    restrictFrame(event);
  });
  contents.on('did-start-navigation', (event) => {
    if (event.isMainFrame && !event.isSameDocument) deviceUi = null;
  });
  contents.on('render-process-gone', () => { deviceUi = null; });
  contents.on('destroyed', () => { deviceUi = null; });
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
    } else if ((url.protocol === 'https:' || url.protocol === 'http:') && url.origin !== origin) void shell.openExternal(target);
    return { action: 'deny' };
  });
  await win.loadURL(`data:text/html;charset=utf-8,${encodeURIComponent(STARTING_PAGE)}`);
  origin = await serving;
  await rendererSession.cookies.set({ url: origin, name: 'orca_access', httpOnly: true, sameSite: 'strict',
    value: createHmac('sha256', ACCESS_TOKEN).update('orca-web-access-v1').digest('hex') });
  if (win && !closing) await win.loadURL(origin);
}

app.on('window-all-closed', () => app.quit());
app.on('activate', () => { if (win && !closing) { win.show(); win.focus(); } });
app.on('before-quit', (e) => {
  if (quitReady) { quitting = true; return; }
  e.preventDefault();
  void prepareQuit();
});
process.on('exit', () => { if (!WINDOWS) server?.kill('SIGTERM'); });
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
    };
    const finished = await Promise.race([
      flush().then(() => true),
      new Promise<false>((resolve) => { timer = setTimeout(() => resolve(false), 10000); }),
    ]);


    if (!finished) throw new Error('Still saving your work! We\'ll keep the window open for a moment—try closing it again once saving finishes.');
    win?.hide();
    await win?.loadURL('about:blank').catch(() => {});
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
