import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { DEFAULT_SLICE_TIMEOUT_S, isSliceTimeout } from './slice-timeout.js';

export const MCP_SERVER_NAME = 'pancakeslicer';

export interface AgentSettings { enabled: boolean; printers: boolean; sliceTimeoutS: number; customConfigs?: string[] }
export interface McpLaunch { command: string; args: string[]; env: Record<string, string> }
export interface AgentClient { id: string; name: string; available: boolean; configured: boolean; current: boolean }

const settingsFile = (userData: string) => path.join(userData, 'agents.json');
export const connectionFile = (userData: string) => path.join(userData, 'agent-connection.json');

export function readAgentSettings(userData: string): AgentSettings {
  try {
    const raw = JSON.parse(fs.readFileSync(settingsFile(userData), 'utf8')) as Partial<AgentSettings>;
    return {
      enabled: raw.enabled === true,
      printers: raw.enabled === true && raw.printers === true,
      sliceTimeoutS: isSliceTimeout(raw.sliceTimeoutS) ? raw.sliceTimeoutS : DEFAULT_SLICE_TIMEOUT_S,
      customConfigs: Array.isArray(raw.customConfigs) ? raw.customConfigs.filter((file) => typeof file === 'string' && path.isAbsolute(file)) : [],
    };
  } catch {
    return { enabled: false, printers: false, sliceTimeoutS: DEFAULT_SLICE_TIMEOUT_S };
  }
}

function writeJsonAtomic(file: string, value: unknown, mode?: number) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  const temp = `${file}.${process.pid}.tmp`;
  fs.writeFileSync(temp, JSON.stringify(value, null, 2) + '\n', { mode });
  fs.renameSync(temp, file);
}

export function writeAgentSettings(userData: string, next: AgentSettings): AgentSettings {
  const settings = { ...next, enabled: next.enabled === true, printers: next.enabled === true && next.printers === true };
  writeJsonAtomic(settingsFile(userData), settings);
  return settings;
}

export function publishConnection(userData: string, settings: AgentSettings, url: string, token: string, control?: { url: string; token: string }) {
  if (!settings.enabled || !url) {
    removeConnection(userData);
    return;
  }
  writeJsonAtomic(connectionFile(userData), { url, token, printers: settings.printers, sliceTimeoutS: settings.sliceTimeoutS, pid: process.pid, ...(control ? { control: control.url, controlToken: control.token } : {}) }, 0o600);
}

export function removeConnection(userData: string) {
  fs.rmSync(connectionFile(userData), { force: true });
}

export function mcpLaunch(execPath: string, script: string, userData: string): McpLaunch {
  return { command: execPath, args: [script, '--connection', connectionFile(userData)], env: { ELECTRON_RUN_AS_NODE: '1' } };
}

function claudeDesktopConfig() {
  const home = os.homedir();
  if (process.platform === 'win32') return path.join(process.env.APPDATA ?? path.join(home, 'AppData', 'Roaming'), 'Claude', 'claude_desktop_config.json');
  if (process.platform === 'darwin') return path.join(home, 'Library', 'Application Support', 'Claude', 'claude_desktop_config.json');
  return path.join(process.env.XDG_CONFIG_HOME ?? path.join(home, '.config'), 'Claude', 'claude_desktop_config.json');
}

const CLIENTS = [
  { id: 'claude-desktop', name: 'Claude Desktop', file: claudeDesktopConfig },
  { id: 'claude-code', name: 'Claude Code', file: () => path.join(os.homedir(), '.claude.json') },
  { id: 'cursor', name: 'Cursor', file: () => path.join(os.homedir(), '.cursor', 'mcp.json') },
];

export function formatMcpConfig(launch: McpLaunch, format: unknown) {
  if (format === 'codex') {
    return [
      `[mcp_servers.${MCP_SERVER_NAME}]`,
      `command = ${JSON.stringify(launch.command)}`,
      `args = ${JSON.stringify(launch.args)}`,
      '',
      `[mcp_servers.${MCP_SERVER_NAME}.env]`,
      ...Object.entries(launch.env).map(([key, value]) => `${key} = ${JSON.stringify(value)}`),
      '',
    ].join('\n');
  }
  if (format === undefined || format === 'json') return JSON.stringify({ mcpServers: { [MCP_SERVER_NAME]: launch } }, null, 2);
  throw new Error('Unknown MCP configuration format.');
}

function readConfig(file: string): Record<string, any> | null {
  try {
    const value = JSON.parse(fs.readFileSync(file, 'utf8'));
    return value && typeof value === 'object' && !Array.isArray(value) ? value : null;
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === 'ENOENT') return {};
    return null;
  }
}

const sameLaunch = (entry: any, launch: McpLaunch) => !!entry && entry.command === launch.command
  && JSON.stringify(entry.args ?? []) === JSON.stringify(launch.args) && entry.env?.ELECTRON_RUN_AS_NODE === '1';

const execFileAsync = promisify(execFile);
const CODEX = 'codex';
const codexProcessOptions = { windowsHide: true, cwd: os.homedir() };

function codexExecutable() {
  const directories = (process.env.PATH ?? '').split(path.delimiter).filter((directory) => path.isAbsolute(directory));
  const windows = process.platform === 'win32';
  if (windows && process.env.LOCALAPPDATA)
    directories.push(path.join(process.env.LOCALAPPDATA, 'Programs', 'OpenAI', 'Codex', 'bin'));
  return directories.map((directory) => path.join(directory, windows ? 'codex.exe' : 'codex')).find((file) => {
    try {
      fs.accessSync(file, fs.constants.X_OK);
      return fs.statSync(file).isFile();
    } catch {
      return false;
    }
  });
}

async function getCodexClient(launch: McpLaunch): Promise<AgentClient> {
  const executable = codexExecutable();
  const client = { id: CODEX, name: 'ChatGPT / Codex', available: !!executable, configured: false, current: false };
  if (!executable) return client;
  try {
    const { stdout } = await execFileAsync(executable, ['mcp', 'get', MCP_SERVER_NAME, '--json'], { ...codexProcessOptions, timeout: 5000 });
    const entry = JSON.parse(stdout);
    return { ...client, configured: true, current: entry.enabled !== false && sameLaunch(entry.transport, launch) };
  } catch {
    return client;
  }
}

async function configureCodex(launch: McpLaunch | null) {
  const executable = codexExecutable();
  if (!executable) throw new Error('Codex CLI was not detected.');
  const args = launch
    ? ['mcp', 'add', MCP_SERVER_NAME, ...Object.entries(launch.env).flatMap(([key, value]) => ['--env', `${key}=${value}`]), '--', launch.command, ...launch.args]
    : ['mcp', 'remove', MCP_SERVER_NAME];
  await execFileAsync(executable, args, { ...codexProcessOptions, timeout: 10000 });
}

function clients(customConfigs: string[]) {
  return [...CLIENTS, ...customConfigs.map((file) => ({ id: `file:${file}`, name: path.basename(file), file: () => file }))];
}

function serverKey(config: Record<string, any>, id: string) {
  return id.startsWith('file:') && !Object.hasOwn(config, 'mcpServers') && Object.hasOwn(config, 'servers') ? 'servers' : 'mcpServers';
}

export async function agentClients(launch: McpLaunch, customConfigs: string[] = []): Promise<AgentClient[]> {
  const known = clients(customConfigs).map((client) => {
    const file = client.file();
    const available = client.id === 'claude-code' ? fs.existsSync(file) || fs.existsSync(path.join(os.homedir(), '.claude')) : fs.existsSync(path.dirname(file));
    const config = available ? readConfig(file) : null;
    const entry = config?.[serverKey(config, client.id)]?.[MCP_SERVER_NAME];
    return { id: client.id, name: client.name, available, configured: !!entry, current: sameLaunch(entry, launch) };
  });
  return [...known, await getCodexClient(launch)];
}

export async function configureAgentClient(id: string, launch: McpLaunch | null, customConfigs: string[] = []) {
  if (id === CODEX) return configureCodex(launch);
  const client = clients(customConfigs).find((c) => c.id === id);
  if (!client) throw new Error('Unknown AI client.');
  const file = client.file();
  const config = readConfig(file);
  if (!config) throw new Error(`${client.name}'s settings file (${file}) isn't valid JSON, so it was left alone.`);
  const key = serverKey(config, id);
  if (!launch && !config[key]?.[MCP_SERVER_NAME]) return;
  const servers = config[key] ?? {};
  if (typeof servers !== 'object' || Array.isArray(servers)) throw new Error(`The ${key} field in ${file} must be a JSON object.`);
  if (launch) servers[MCP_SERVER_NAME] = id === 'claude-code' || key === 'servers' ? { type: 'stdio', ...launch } : launch;
  else delete servers[MCP_SERVER_NAME];
  config[key] = servers;
  writeJsonAtomic(file, config);
}
