import { randomId } from './random-id';

export const pageId: string = randomId().replace(/-/g, '');
type Envelope = { topic: string; body: unknown };
const routes = new Map<string, (body: never) => void>();
const channel = new BroadcastChannel('pancake-tabs');
channel.onmessage = (e: MessageEvent<Envelope>) => routes.get(e.data.topic)?.(e.data.body as never);

export function tabTopic<T>(topic: string, receive: (body: T) => void): (body: T) => void {
  if (routes.has(topic)) throw new Error(`tab topic ${topic} is taken`);
  routes.set(topic, receive);
  return (body) => channel.postMessage({ topic, body } satisfies Envelope);
}
