#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path
import zipfile

import geometry


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, indent=2)+'\n', encoding='utf-8')


def relative_to_stage(value, root):
    if isinstance(value, str):
        return value.replace(root + '/', '')
    if isinstance(value, list):
        return [relative_to_stage(item, root) for item in value]
    if isinstance(value, dict):
        return {key: relative_to_stage(item, root) for key, item in value.items()}
    return value


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--engine', required=True, type=Path)
    ap.add_argument('--qualification', required=True, type=Path)
    ap.add_argument('--evidence-archive', required=True, type=Path)
    args = ap.parse_args()
    stage_root = str(args.engine.resolve().parent)
    proof = relative_to_stage(json.loads((args.qualification/'qualification.json').read_text()), stage_root)
    if not proof.get('ready'):
        raise RuntimeError('Qualification did not pass; no ready manifest is written')
    cross_platform = proof['platform'] != {'os':'linux','arch':'x86_64'}
    if cross_platform and not all(proof['identity'][name].get('geometry',{}).get('passed') is True for name in ['benchy_flat','benchy']):
        raise RuntimeError('Cross-platform flat and curved Benchy geometric equivalence did not pass')
    if not cross_platform and proof['identity']['benchy_flat']['gcode']['identity'] not in {'byte','normalized'}:
        raise RuntimeError('Same-platform flat Benchy identity did not pass')
    equivalence = {name:geometry.summary(proof['identity'][name]) for name in ['benchy_flat','benchy']} if cross_platform else None
    entries = {}
    for name, manifest_name in [('benchy','benchy'),('dome_lid','dome'),('coaster','wave')]:
        item = proof['audits'][name]
        verifier = item.get('verifier', {})
        if not item['ok'] or item['passed'] != item['total'] or item['total'] != item['expected'] or item['hit_moves'] != 0 or verifier.get('hit_moves') != 0 or verifier.get('machine_hit_moves') != 0:
            raise RuntimeError(f'{name}: incomplete or failed qualification')
        comparison = proof['identity'][name]
        identity = {'reference_platform':'linux/x86_64', 'reference_gcode_sha256':comparison['reference_gcode_sha256'],
                    'mode': {'byte':'byte','normalized':'normalized','diff':'differs'}[comparison['gcode']['identity']],
                    'package':comparison['package']}
        if identity['mode'] == 'normalized':
            identity['rule'] = comparison['gcode']['rule']
            if 'normalized_sha256' in comparison['gcode']:
                identity['normalized_sha256'] = comparison['gcode']['normalized_sha256']
        if identity['mode'] == 'differs':
            identity['diff'] = comparison['gcode']
        if cross_platform and name == 'benchy':
            identity = {**identity,**equivalence[name], 'byte_identity':comparison['gcode']['identity']}
        entries[manifest_name] = {'native_qualified':True,'full_audit_qualified':True,
            'audit_checks_passed':item['passed'],'audit_checks_total':item['total'],
            'gcode_sha256':item['gcode_sha256'],'native_package_sha256':item['package_sha256'],
            'clearance_hit_moves':0,'verifier':verifier,'identity':identity}
    binding = {'source_sha':proof['engine_source_sha'],'publication_main_sha':proof['publication_main_sha'],
               'platform':proof['platform'],'binary_sha256':proof['binary_sha256'],
               'resources_sha256':proof['resources_sha256']}
    manifest = {**binding,'schema':3,'backend':'native','available':True,'preview_ready':True,'print_qualified':True,
                'resource_file_count':proof['resource_file_count'],'validated_curved_benchy_clearance':True,
                'validated_wave_export':True,'features':{'curved_tops':True,'nonplanar_wave':True},**entries}
    if cross_platform:
        manifest['cross_platform_equivalence'] = equivalence
    manifest_path = args.engine/'surface-manifest.json'
    write(manifest_path,manifest)
    args.evidence_archive.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(args.evidence_archive,'w',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
        for path in sorted(args.qualification.rglob('*')):
            if not path.is_file() or path.suffix == '.stl' or 'data' in path.relative_to(args.qualification).parts:
                continue
            info=zipfile.ZipInfo(path.relative_to(args.qualification).as_posix(),(2020,1,1,0,0,0))
            info.compress_type=zipfile.ZIP_DEFLATED
            info.external_attr=0o100644<<16
            archive.writestr(info,path.read_bytes().replace((stage_root+'/').encode(),b''))
    ready = {**binding,'schema':'orca.ns7.qualification.v2','ready':True,'manifest_sha256':digest(manifest_path),
             'outputs':{name:entries[alias] for name,alias in [('benchy','benchy'),('dome_lid','dome'),('coaster','wave')]},
             'evidence':{'archive_sha256':digest(args.evidence_archive)}}
    if cross_platform:
        ready['cross_platform_equivalence'] = equivalence
    write(args.engine/'artifact-qualification.ready',ready)
    print(json.dumps({'source_sha':binding['source_sha'],'publication_main_sha':binding['publication_main_sha'],
                      'platform':binding['platform'],'ready':True,'evidence_sha256':digest(args.evidence_archive)},indent=2))


if __name__ == '__main__':
    main()
