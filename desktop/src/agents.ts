import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

export const MCP_SERVER_NAME = 'pancakeslicer';

export interface AgentSettings { enabled: boolean; printers: boolean }
export interface McpLaunch { command: string; args: string[]; env: Record<string, string> }
export interface AgentClient { id: string; name: string; available: boolean; configured: boolean; current: boolean }

const settingsFile = (userData: string) => path.join(userData, 'agents.json');
export const connectionFile = (userData: string) => path.join(userData, 'agent-connection.json');

export function readAgentSettings(userData: string): AgentSettings {
  try {
    const raw = JSON.parse(fs.readFileSync(settingsFile(userData), 'utf8')) as Partial<AgentSettings>;
    return { enabled: raw.enabled === true, printers: raw.enabled === true && raw.printers === true };
  } catch {
    return { enabled: false, printers: false };
  }
}

function writeJsonAtomic(file: string, value: unknown, mode?: number) {
  fs.mkdirSync(path.dirname(file), { recursive: true });
  const temp = `${file}.${process.pid}.tmp`;
  fs.writeFileSync(temp, JSON.stringify(value, null, 2) + '\n', { mode });
  fs.renameSync(temp, file);
}

export function writeAgentSettings(userData: string, next: AgentSettings): AgentSettings {
  const settings = { enabled: next.enabled === true, printers: next.enabled === true && next.printers === true };
  writeJsonAtomic(settingsFile(userData), settings);
  return settings;
}

export function publishConnection(userData: string, settings: AgentSettings, url: string, token: string) {
  if (!settings.enabled || !url) {
    removeConnection(userData);
    return;
  }
  writeJsonAtomic(connectionFile(userData), { url, token, printers: settings.printers, pid: process.pid }, 0o600);
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

export function agentClients(launch: McpLaunch): AgentClient[] {
  return CLIENTS.map((client) => {
    const file = client.file();
    const available = client.id === 'claude-code' ? fs.existsSync(file) || fs.existsSync(path.join(os.homedir(), '.claude')) : fs.existsSync(path.dirname(file));
    const entry = available ? readConfig(file)?.mcpServers?.[MCP_SERVER_NAME] : undefined;
    return { id: client.id, name: client.name, available, configured: !!entry, current: sameLaunch(entry, launch) };
  });
}

export function addToClient(id: string, launch: McpLaunch) {
  const client = CLIENTS.find((c) => c.id === id);
  if (!client) throw new Error('Unknown AI client.');
  const file = client.file();
  const config = readConfig(file);
  if (!config) throw new Error(`${client.name}'s settings file (${file}) isn't valid JSON, so it was left alone.`);
  const servers = config.mcpServers && typeof config.mcpServers === 'object' ? config.mcpServers : {};
  servers[MCP_SERVER_NAME] = id === 'claude-code' ? { type: 'stdio', ...launch } : launch;
  config.mcpServers = servers;
  writeJsonAtomic(file, config);
}

export function removeFromClient(id: string) {
  const client = CLIENTS.find((c) => c.id === id);
  if (!client) throw new Error('Unknown AI client.');
  const file = client.file();
  const config = readConfig(file);
  if (!config?.mcpServers?.[MCP_SERVER_NAME]) return;
  delete config.mcpServers[MCP_SERVER_NAME];
  writeJsonAtomic(file, config);
}
