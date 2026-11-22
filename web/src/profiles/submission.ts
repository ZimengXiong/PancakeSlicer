import type { PresetType } from '../api/types';
import type { State } from '../state/store';
import { selectCompatibility } from './compatibility';
import { recordOf } from './view';
import { slotFilaments } from './slots';
export function validateProfileSubmission(s:State):void {
  const selections:[PresetType,string,number|undefined][]=[['machine',s.printer,undefined],['process',s.process,undefined],...slotFilaments(s).flatMap((k,i)=>k?[['filament',k,i+1] as [PresetType,string,number]]:[])];
  for(const [type,key,slot] of selections){
    const record=recordOf(s,type,key);
    if(!record)throw Object.assign(new Error('Please save or restore the selected profile before slicing.'),{code:'profile_unknown',recovery:'save_profile',profile:{kind:type,name:key,vendor:'',slot}});
    const compatibility=selectCompatibility(s,type,key);
    if(!compatibility.ok)throw Object.assign(new Error(`${record.name}: ${compatibility.reason}`),{code:'profile_incompatible',recovery:'repair_profile',profile:{kind:type,name:record.name,vendor:'',slot}});
  }
}
