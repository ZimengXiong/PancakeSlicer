import { createServiceDocument, type JsonValue } from './service-document';
import { requireWorkspaceService } from './service';
export type SavedPreferences = Record<string, JsonValue>;
const preferences = createServiceDocument<SavedPreferences>('/api/preferences', 'PUT', 'preferences', requireWorkspaceService);
export const servicePreferencesSnapshot = preferences.snapshot;
export const subscribeServicePreferences = preferences.subscribe;
export async function hydrateServicePreferences() { await requireWorkspaceService(); return preferences.hydrate(); }
export const setServicePreferences = preferences.patch;
export const flushServicePreferences = preferences.flush;
export const reloadServicePreferences = preferences.reload;
