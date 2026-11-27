import assert from 'node:assert/strict';
import { test } from 'node:test';
import type { LibraryRecords, NozzleRecord, ProcessRecord } from '../src/profiles/records.ts';

test('dual-nozzle presets restore each nozzle’s catalog basis after reload', async () => {
  assert.ok(process.env.NOZZLE_TEST_URL, 'Set NOZZLE_TEST_URL to a disposable local server');
  const origin = new URL(process.env.NOZZLE_TEST_URL);
  assert.equal(origin.hostname, '127.0.0.1', 'Use a disposable local server');
  async function api<T>(path: string, method = 'GET', body?: unknown): Promise<T> {
    const response = await fetch(new URL(`/api/${path}`, origin), {
      method, headers: { 'content-type': 'application/json', origin: origin.origin }, body: JSON.stringify(body),
    });
    const text = await response.text();
    assert.ok(response.ok, `${method} ${path}: ${response.status} ${text}`);
    return JSON.parse(text);
  }

  const library = async () => (await api<{ document: LibraryRecords }>('library')).document;
  const document = await library();
  for (const records of [document.printers, document.processes, document.filaments, document.calibrations])
    assert.equal(records.length, 0, 'Use a disposable server with an empty library');
  const prefs = await api<{ revision: number }>('preferences');
  assert.equal(prefs.revision, 0, 'Use a disposable server with empty preferences');

  const model = 'Bambu Lab H2D';
  const bases = [`${model} 0.4 nozzle`, `${model} 0.6 nozzle`];
  const preset = { diameter: 0.5, variants: bases };
  const print = (await api<{ record: ProcessRecord }[]>('print-catalog')).find(({ record }) => record.id === 'profile:starter-everyday')!.record;
  const filaments = ['Bambu PLA Basic', 'Bambu PETG HF'].map((product, i) => ({ id: `profile:filament-${i}`, product, name: product, revision: 1 }));
  const dual = {
    id: 'profile:dual', model, vendor: 'BBL', name: 'Dual H2D', revision: 1, swap: 'ams' as const, process: print.id,
    nozzles: bases.map((variant) => ({ variant, diameter: 0.4 })),
    slots: ['ext.L', 'ext.R'].map((tray, i) => ({ id: `slot:${i}`, tray, filament: filaments[0].id })),
  };
  const legacy = {
    ...dual, id: 'profile:legacy', name: 'Legacy H2D',
    nozzles: bases.map(() => ({ variant: '', diameter: 0.55 })),
    slots: dual.slots.map((slot, i) => ({ ...slot, id: `slot:legacy-${i}` })),
  };
  await api('library', 'PUT', {
    operationId: crypto.randomUUID(), expectedRevision: document.revision,
    document: { ...document, printers: [dual, legacy], processes: [print], filaments },
  });
  await api('preferences', 'PUT', { operationId: crypto.randomUUID(), expectedRevision: prefs.revision, document: { 'orca.setup': '1' } });

  const { chromium } = await import(process.env.PLAYWRIGHT_CORE ?? 'playwright-core');
  const browser = await chromium.launch({ executablePath: process.env.CHROME, args: ['--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
  try {
    const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
    page.setDefaultTimeout(15_000);
    const size = page.getByRole('combobox', { name: 'Size size', exact: true });
    async function openPrinter(name: string) {
      await page.waitForFunction(() => (window as any).__plater?.getState().profileLibraryLoad?.state === 'ready');
      await page.getByRole('button', { name: 'Device', exact: true }).click();
      await page.getByRole('button', { name: new RegExp(name) }).click();
      await size.waitFor();
    }
    async function choose(label: RegExp) {
      await size.click();
      await page.getByRole('option', { name: label }).click();
    }
    async function switched(name: string, diameter: number) {
      await page.locator('.toast').getByText(`${name}: ${diameter} mm nozzle`, { exact: true }).waitFor();
      await size.filter({ hasText: `${diameter} mm` }).waitFor();
    }
    const printer = async (id: string) => (await library()).printers.find((p) => p.id === id)!;
    const nozzlePairs = (nozzles: NozzleRecord[]) => nozzles.map(({ variant, diameter }) => [variant, diameter]);

    await page.goto(origin.href);
    await openPrinter(dual.name);
    await choose(/^Other size/);
    await page.getByRole('spinbutton', { name: 'Size diameter in mm' }).fill('0.5');
    await page.getByRole('button', { name: 'Apply', exact: true }).click();
    await switched(dual.name, 0.5);
    assert.deepEqual((await printer(dual.id)).nozzlePresets, [preset]);

    await choose(/^0\.8 mm$/);
    await switched(dual.name, 0.8);
    assert.deepEqual(nozzlePairs((await printer(dual.id)).nozzles), Array.from({ length: 2 }, () => [`${model} 0.8 nozzle`, 0.8]));
    await page.reload();
    await openPrinter(dual.name);
    await size.click();
    await page.getByRole('option', { name: '0.5 mm', exact: true }).filter({ hasText: 'based on 0.4 / 0.6 mm' }).click();
    await switched(dual.name, 0.5);
    assert.deepEqual(nozzlePairs((await printer(dual.id)).nozzles), bases.map((variant) => [variant, 0.5]));

    const { printer: resolved } = await api<{ printer: { status: { ok: boolean }; values: Record<string, string[]> } }>('resolve', 'POST', {
      printer: dual.id, print: print.id, slots: dual.slots.map((slot) => slot.id), filaments: dual.slots.map((slot) => slot.filament), usedSlots: [1, 2],
    });
    assert.equal(resolved.status.ok, true, JSON.stringify(resolved.status));
    assert.deepEqual(resolved.values.min_layer_height.map(Number), [0.08, 0.12]);

    const savedSlot = page.waitForResponse((response) => response.url().endsWith('/api/library') && response.request().method() === 'PUT' && response.ok());
    await page.getByRole('combobox', { name: /filament$/i }).first().click();
    await page.getByRole('option', { name: filaments[1].name }).click();
    await savedSlot;
    await page.locator('.toast', { hasText: `${dual.name}: 0.5 mm nozzle` }).getByRole('button', { name: 'Undo', exact: true }).click();
    await page.getByRole('alert').filter({ hasText: 'Can’t undo: this printer changed after the nozzle was switched.' }).waitFor();
    const edited = await printer(dual.id);
    assert.equal(edited.slots[0].filament, filaments[1].id);
    assert.deepEqual(nozzlePairs(edited.nozzles), bases.map((variant) => [variant, 0.5]));

    await page.getByRole('button', { name: /Legacy H2D/ }).click();
    await choose(/^0\.4 mm$/);
    await switched(legacy.name, 0.4);
    const repaired = await printer(legacy.id);
    assert.deepEqual(nozzlePairs(repaired.nozzles), Array.from({ length: 2 }, () => [bases[0], 0.4]));
    assert.deepEqual(repaired.nozzlePresets ?? [], []);
  } finally {
    await browser.close();
  }
});
