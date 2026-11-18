import { mutation, responseError } from '../api/access';
import { LIBRARY_SCHEMA, type LibraryRecords } from '../profiles/records';
import { setState, type State } from '../state/store';

export interface LibraryProblem { code: string; message: string }
export interface LibraryHealth { problems: Record<string, LibraryProblem>; notices: LibraryProblem[] }
interface LibraryAnswer { document: LibraryRecords; problems: Record<string, LibraryProblem>; notices: LibraryProblem[] }

export function problemText(problem: LibraryProblem | undefined, name: string): string | undefined {
  if (!problem) return undefined;
  const text = problem.message.startsWith(`${name}: `) ? problem.message.slice(name.length + 2) : problem.message;
  return text.charAt(0).toUpperCase() + text.slice(1);
}

export function recordProblems(s: Pick<State, 'libraryHealth' | 'records'>, kind: 'machine' | 'process' | 'filament', id: string, name: string): string[] {
  const { problems } = s.libraryHealth, out = [problemText(problems[id], name)];
  if (kind !== 'process') for (const c of s.records.calibrations) {
    const problem = problems[c.id];
    if (!problem || (kind === 'filament' ? c.filament : c.printer) !== id) continue;
    const filament = s.records.filaments.find((f) => f.id === c.filament)?.name ?? '', printer = s.records.printers.find((p) => p.id === c.printer)?.name ?? '';
    const owner = `${filament} on ${printer}: `, rest = problem.message.startsWith(owner) ? problem.message.slice(owner.length) : problem.message;
    out.push(kind === 'filament' ? `On ${printer}: ${rest}` : `${filament}: ${rest}`);
  }
  return out.filter((t): t is string => !!t);
}

function adopted(answer: LibraryAnswer): LibraryRecords {
  const doc = checked(answer.document);
  setState({ libraryHealth: { problems: answer.problems ?? {}, notices: answer.notices ?? [] } });
  return doc;
}

export const CLIENT_OUTDATED = 'PancakeSlicer got an update!';

function checked(doc: LibraryRecords): LibraryRecords {
  if (typeof (doc as { schema?: unknown } | null)?.schema === 'number' && doc.schema > LIBRARY_SCHEMA) throw new Error(CLIENT_OUTDATED);
  if (!doc || doc.schema !== LIBRARY_SCHEMA || !Number.isInteger(doc.revision) || !Array.isArray(doc.printers) || !Array.isArray(doc.filaments)
    || !Array.isArray(doc.processes) || !Array.isArray(doc.calibrations))
    throw new Error(`Oops, this library document isn't schema ${LIBRARY_SCHEMA} (got schema ${String((doc as { schema?: unknown } | null)?.schema)}, revision ${String(doc?.revision)}).`);
  return doc;
}

export async function readLibrary(): Promise<LibraryRecords> {
  const response = await fetch('/api/library');
  if (!response.ok) throw await responseError(response);
  return adopted(await response.json() as LibraryAnswer);
}
export const encodeLibraryWrite = (operationId: string, expectedRevision: number, document: LibraryRecords): string =>
  JSON.stringify({ operationId, expectedRevision, document });
export async function putLibrary(body: string): Promise<LibraryRecords> {
  const response = await mutation('/api/library', { method: 'PUT', headers: { 'Content-Type': 'application/json' }, body }, false, undefined, undefined, true);
  if (!response.ok) throw await responseError(response);
  return adopted(await response.json() as LibraryAnswer);
}
