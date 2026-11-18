import { toolArtifactManifest } from '../api/client';
import { effectiveConfig, resolvePreset, type State } from '../state/store';
import { deriveMethodAvailability } from './capabilities';

export function methodAvailability(state: State, printer = effectiveConfig(state, 'machine')) {
  const resolved = resolvePreset(state, 'machine', state.printer);
  return deriveMethodAvailability({printer:{known:!!resolved.base && !resolved.error,config:printer},engine:{
    artifact_manifest:toolArtifactManifest}});
}
