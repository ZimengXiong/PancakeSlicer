import { serviceAnswered } from './api/access';
import { librarySync } from './flow/library-sync';
import { pageId as id, tabTopic } from './tabs';

export type LiveEvent = 'revision' | 'connections' | 'session' | 'down' | 'up';
type Message = { kind: 'event'; type: LiveEvent; data: string } | { kind: 'hello' }
  | { kind: 'beat' | 'claim' | 'bye'; term: number; id: string };

const BEAT_MS = 1_000, DEAD_MS = 3_000, CLAIM_MS = 250;
const handlers = new Map<LiveEvent, Set<(data: string) => void>>();
const latest = new Map<LiveEvent, string>();
let started = false;
let role: 'follower' | 'claimant' | 'leader' = 'follower', term = 0, heardLeader = 0, beaten = 0;

const deliver = (type: LiveEvent, data: string) => {
  const lost = librarySync.get().value === 'reconnecting';
  if (type === 'down' && !lost) librarySync.send({ type: 'STREAM_LOST' }); else if (type !== 'down' && lost) librarySync.send({ type: 'STREAM_BACK' });
  if (type !== 'down' && type !== 'up') latest.set(type, data);
  handlers.get(type)?.forEach((h) => h(data));
};
const publish = (type: LiveEvent, data: string) => { deliver(type, data); post({ kind: 'event', type, data }); };
const beat = () => { beaten = Date.now(); post({ kind: 'beat', term, id }); };
const outranks = (t: number, other: string) => t > term || (t === term && other < id);

let current: { source: EventSource; watch: ReturnType<typeof setInterval> } | null = null;
function close() { if (current) { current.source.close(); clearInterval(current.watch); current = null; } }
function open() {
  if (role !== 'leader') return;
  close();
  const source = new EventSource('/api/events');
  let heard = Date.now();
  const lost = () => {
    if (current?.source !== source) return;
    close();
    publish('down', '');
  };
  const watch = setInterval(() => { if (Date.now() - heard > 1_500) lost(); }, 250);
  current = { source, watch };
  const alive = () => { heard = Date.now(); beat(); serviceAnswered(); if (librarySync.get().value === 'reconnecting') publish('up', ''); };
  source.addEventListener('ping', alive);
  for (const type of ['revision', 'connections', 'session'] as const) source.addEventListener(type, (e) => { alive(); publish(type, (e as MessageEvent<string>).data); });
  source.onerror = lost;
}

function follow() { if (role === 'leader') close(); role = 'follower'; heardLeader = Date.now(); }
function claim() {
  role = 'claimant'; term += 1;
  post({ kind: 'claim', term, id });
  const mine = term;
  setTimeout(() => { if (role === 'claimant' && term === mine) { role = 'leader'; beat(); open(); } }, CLAIM_MS);
}
function leave() { if (role === 'leader') { close(); post({ kind: 'bye', term, id }); } role = 'follower'; heardLeader = Date.now(); }
function rejoin() { heardLeader = Date.now() - DEAD_MS + 2 * CLAIM_MS; post({ kind: 'hello' }); }

function receive(m: Message) {
  if (m.kind === 'event') { heardLeader = Date.now(); deliver(m.type, m.data); return; }
  if (m.kind === 'hello') { if (role === 'leader') { beat(); for (const [type, data] of latest) post({ kind: 'event', type, data }); } return; }
  if (m.kind === 'bye') { if (role === 'follower') claim(); return; }
  if (outranks(m.term, m.id)) { term = m.term; follow(); }
  else if (role === 'leader') beat();
  else if (m.kind === 'beat') heardLeader = Date.now();
}
const post = tabTopic<Message>('events', (m) => { if (started) receive(m); });

function start() {
  if (started || import.meta.env.MODE === 'demo') return;
  started = true;
  librarySync.send({ type: 'STREAM_OPENER', open });
  setInterval(() => {
    const now = Date.now();
    if (role === 'follower' && now - heardLeader > DEAD_MS) claim();
    else if (role === 'leader' && now - beaten >= BEAT_MS) beat();
  }, CLAIM_MS);
  window.addEventListener('pagehide', leave);
  document.addEventListener('freeze', leave);
  document.addEventListener('resume', rejoin);
  window.addEventListener('pageshow', (e) => { if (e.persisted) rejoin(); });
  rejoin();
}

export function onLiveEvent(type: LiveEvent, handler: (data: string) => void): () => void {
  let set = handlers.get(type);
  if (!set) handlers.set(type, set = new Set());
  set.add(handler);
  const last = latest.get(type);
  if (last !== undefined) handler(last);
  start();
  return () => { set!.delete(handler); };
}
