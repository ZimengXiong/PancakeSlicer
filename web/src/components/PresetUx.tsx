import { Suspense } from 'react';
import { useStore } from '../state/store';
import { preloadable } from '../preloadable';

const ImportDialog = preloadable(() => import('./PresetIO').then((m) => ({ default: m.ImportDialog })));
const ExportDialog = preloadable(() => import('./PresetIO').then((m) => ({ default: m.ExportDialog })));

export default function PresetUx() {
  const ux = useStore((s) => s.presetUx);
  if (!ux) return null;
  return (
    <Suspense fallback={<div role="status">Loading…</div>}>
      {ux.kind === 'import' && <ImportDialog />}
      {ux.kind === 'export' && <ExportDialog />}
    </Suspense>
  );
}
