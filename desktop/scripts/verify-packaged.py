#!/usr/bin/env python3
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import zipfile

ROOT=Path(__file__).resolve().parents[2]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream,'sha256').hexdigest()


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--archive',required=True,type=Path)
    parser.add_argument('--evidence',required=True,type=Path)
    args=parser.parse_args()
    spec=importlib.util.spec_from_file_location('staging',ROOT/'desktop/scripts/stage-ci.py')
    stage=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(stage)
    with tempfile.TemporaryDirectory(prefix='pancake-packaged-') as temporary:
        root=Path(temporary)
        with zipfile.ZipFile(args.archive) as archive:
            prefixes=[name[:-len('engine/COMPONENTS.json')] for name in archive.namelist() if name.endswith('/engine/COMPONENTS.json')]
            if len(prefixes)!=1:
                raise RuntimeError('Archive must contain one installed payload')
            prefix=prefixes[0]
            for member in archive.infolist():
                if not member.filename.startswith(prefix):continue
                relative=Path(member.filename[len(prefix):])
                if not relative.parts or relative.is_absolute() or '..' in relative.parts:
                    raise RuntimeError('Invalid package member')
                if relative.parts[0] not in {'engine','web'}:continue
                if member.is_dir():continue
                destination=root/relative
                destination.parent.mkdir(parents=True,exist_ok=True)
                with archive.open(member) as source,destination.open('wb') as output:
                    shutil.copyfileobj(source,output)
        engine=root/'engine'
        components=json.loads((engine/'COMPONENTS.json').read_text())
        manifest=json.loads((engine/'surface-manifest.json').read_text())
        ready=json.loads((engine/'artifact-qualification.ready').read_text())
        exe='.exe' if (engine/'nonplanar_slice_tool.exe').is_file() else ''
        for name,key in [('nonplanar_slice_tool'+exe,'native_binary_sha256'),('orca-webapp-server'+exe,'server_binary_sha256'),('surface-manifest.json','native_manifest_sha256'),('artifact-qualification.ready','native_qualification_sha256'),('support-fins/support-fins.mjs','support_fins_sha256')]:
            if digest(engine/name)!=components[key]:raise RuntimeError('Packaged file changed after qualification: '+name)
        if exe and digest(engine/'pancakeslicer-server-guard.exe')!=components['server_guard_sha256']:
            raise RuntimeError('Packaged server guard changed after qualification')
        resources,count=stage.resource_identity(engine/'resources')
        if resources!=components['native_resources_sha256'] or resources!=manifest['resources_sha256'] or resources!=ready['resources_sha256'] or count!=manifest['resource_file_count']:
            raise RuntimeError('Packaged resources differ from qualification')
        if manifest['binary_sha256']!=components['native_binary_sha256'] or ready['binary_sha256']!=manifest['binary_sha256'] or ready['manifest_sha256']!=digest(engine/'surface-manifest.json') or not ready['ready'] or not manifest['print_qualified']:
            raise RuntimeError('Packaged manifest binding is incomplete')
        if ready['evidence']['archive_sha256'] != digest(args.evidence):
            raise RuntimeError('Qualification evidence archive differs')
        expected_platform={'os':'windows' if exe else 'darwin','arch':'x86_64' if exe else 'arm64'}
        for record in [manifest,ready]:
            if record['source_sha']!=components['native_source'] or record['publication_main_sha']!=components['server_source'] or record['platform']!=expected_platform:
                raise RuntimeError('Packaged source or platform binding differs')
        schemas=json.loads((engine/'schemas.json').read_text())
        if not schemas or not all(type(value) is int and value>0 for value in schemas.values()):
            raise RuntimeError('Packaged server schemas are missing')
        web=json.loads((root/'web/COMPONENTS.json').read_text())
        if web!=components or (root/'web/REVISION.txt').read_text().strip()!=components['client_source']:
            raise RuntimeError('Packaged client identity differs')
        print(json.dumps({'archive':args.archive.name,'binary_sha256':manifest['binary_sha256'],'resources_sha256':resources,'resource_file_count':count,'ready':True},indent=2))


if __name__=='__main__':
    main()
