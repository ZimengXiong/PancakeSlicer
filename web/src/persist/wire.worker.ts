import { prepareDocumentText, parseDocument, sha256 } from './wire';
import type { WireRequest, WireResponse } from './wire-protocol';

const captures = new Map<number, { value: Record<string, unknown>; stringify: (value: unknown) => string }>();
const post = (message: WireResponse) => (self as unknown as Worker).postMessage(message);
const capture = (id: number) => {
  if (!captures.has(id)) throw new Error('Project wire preparation expired. Keep the document and retry.');
  return captures.get(id)!;
};
(self as unknown as Worker).onmessage = async (event: MessageEvent<WireRequest>) => {
  const request = event.data;
  if (request.type === 'release') { captures.delete(request.capture); return; }
  try {
    switch (request.type) {
      case 'capture': {
        const prepared = prepareDocumentText(request.value);
        const digest = await sha256(new TextEncoder().encode(prepared.text));
        captures.set(request.id, { value: request.value as Record<string, unknown>, stringify: prepared.stringify });
        post({ type: 'digest', id: request.id, digest }); break;
      }
      case 'project': {
        const prepared = capture(request.capture);
        request.body.record.doc = prepared.value.doc;
        post({ type: 'text', id: request.id, text: prepared.stringify(request.body) }); break;
      }
      case 'draft': {
        const prepared = capture(request.capture);
        post({ type: 'text', id: request.id, text: prepared.stringify({ operationId: request.operationId, ...prepared.value }) }); break;
      }
      case 'parse': post({ type: 'parsed', id: request.id, value: parseDocument(request.text) }); break;
      default: { const never: never = request; throw new Error(`Invalid project wire operation: ${String(never)}`); }
    }
  } catch (error) {
    if (request.type === 'capture') captures.delete(request.id);
    post({ type: 'error', id: request.id, name: error instanceof Error ? error.name : 'Error', message: error instanceof Error ? error.message : String(error) });
  }
};
