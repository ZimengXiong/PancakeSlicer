import { getState } from '../state/store';
import { followLibraryRevision } from './transactions';
import { onLiveEvent } from '../live-events';

let following = false, retry: ReturnType<typeof setTimeout> | undefined;

function follow(revision: number, attempt = 0) {
  clearTimeout(retry);
  void followLibraryRevision(revision).then(() => {
    if (revision > getState().profileLibraryRevision && attempt < 10) retry = setTimeout(() => follow(revision, attempt + 1), 1000);
  }, () => {});
}

export function watchLibraryRevisions() {
  if (following || getState().api?.mock) return;
  following = true;
  onLiveEvent('revision', (data) => {
    const revision = Number((JSON.parse(data) as { revision?: unknown }).revision);
    if (Number.isFinite(revision)) follow(revision);
  });
}
