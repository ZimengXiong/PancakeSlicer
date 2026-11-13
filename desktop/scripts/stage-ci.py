#!/usr/bin/env python3
"""Stage a self-contained desktop payload from this checkout's Release engine."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def payload_modes(root, executables):
    for item in [root, *root.rglob('*')]:
        if item.is_dir():
            item.chmod(0o755)
        elif item.is_file():
            item.chmod(0o755 if item in executables or item.suffix.lower() in {'.dll', '.dylib', '.so'} else 0o644)


def resource_identity(root):
    h = hashlib.sha256()
    count = 0
    def visit(directory):
        nonlocal count
        for path in sorted(directory.iterdir(), key=lambda p: p.name):
            if path.is_symlink():
                raise RuntimeError(f'Resource symlink: {path}')
            if path.is_dir():
                visit(path)
            else:
                h.update(f'{path.relative_to(root).as_posix()}\0{digest(path)}\n'.encode())
                count += 1
    visit(root)
    return h.hexdigest(), count


def mac_libraries(tool, prefix, engine):
    """Copy non-system dylibs and make every load command bundle-relative."""
    library_dir = engine / 'lib'
    library_dir.mkdir()
    pending = [(tool, True)]
    copied = set()
    while pending:
        binary, executable = pending.pop()
        lines = subprocess.check_output(['otool', '-L', str(binary)], text=True).splitlines()[1:]
        dependencies = [line.strip().split(' (', 1)[0] for line in lines]
        for dependency in dependencies:
            if dependency.startswith(('/usr/lib/', '/System/Library/')):
                continue
            if not executable and Path(dependency).name == binary.name:
                continue
            name = Path(dependency).name
            source = Path(dependency)
            if not source.is_file():
                candidates = sorted(prefix.rglob(name))
                if not candidates:
                    raise RuntimeError(f'Cannot resolve {dependency} for {binary}')
                source = candidates[0]
            target = library_dir / name
            if name not in copied:
                shutil.copy2(source.resolve(), target)
                target.chmod(0o755)

                run('install_name_tool', '-id', '@loader_path/' + name, target)
                copied.add(name)
                pending.append((target, False))
            relative = '@loader_path/' + ('lib/' if executable else '') + name
            run('install_name_tool', '-change', dependency, relative, binary)
    for library in sorted(library_dir.iterdir()):
        run('strip', '-S', '-x', library)
        run('codesign', '--force', '--sign', '-', library)
    run('strip', '-S', '-x', tool)
    run('codesign', '--force', '--sign', '-', tool)


def windows_libraries(tool, prefix, engine):
    sources = [p for p in prefix.rglob('*.dll') if 'debug' not in {part.lower() for part in p.parts}]
    for directory in ['engine/deps/GMP/gmp/lib/win-x64', 'engine/deps/MPFR/mpfr/lib/win-x64']:
        sources.extend((ROOT / directory).glob('*.dll'))
    redist = os.environ.get('VCToolsRedistDir')
    if not redist:
        raise RuntimeError('VCToolsRedistDir is required to bundle the Visual C++ runtime')
    sources.extend(p for p in (Path(redist) / 'x64').rglob('*.dll')
                   if any(part.startswith('Microsoft.VC') and part.endswith('.CRT') for part in p.parts))
    indexed = {}
    for source in sources:
        name = source.name.lower()
        if name in indexed and digest(indexed[name]) != digest(source):
            raise RuntimeError(f'Conflicting runtime DLL: {source}')
        indexed[name] = source
    system = Path(os.environ['SystemRoot']) / 'System32'
    pending = [tool]
    copied = set()
    while pending:
        binary = pending.pop()
        output = subprocess.check_output(['dumpbin', '/dependents', str(binary)],
                                         text=True, encoding='utf-8', errors='replace')
        for name in re.findall(r'^\s+([A-Za-z0-9_.+\-]+\.dll)\s*$', output, re.M | re.I):
            key = name.lower()
            if key in copied:
                continue
            source = indexed.get(key)
            if source is None:
                if key.startswith(('api-ms-', 'ext-ms-')) or (system / name).is_file():
                    continue
                raise RuntimeError(f'Cannot resolve {name} for {binary}')
            target = engine / source.name
            shutil.copy2(source, target)
            copied.add(key)
            pending.append(target)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--os', choices=['mac', 'win', 'linux'], required=True)
    parser.add_argument('--arch', choices=['arm64', 'x64'], required=True)
    parser.add_argument('--tool', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--engine-source', required=True)
    parser.add_argument('--image')
    parser.add_argument('--dependency-prefix',type=Path)
    args = parser.parse_args()
    tool = args.tool.resolve()
    build = ROOT / 'desktop/build'
    build.mkdir(parents=True, exist_ok=True)
    engine = build / f'engine-{args.os}-{args.arch}'
    if engine.exists():
        shutil.rmtree(engine)
    engine.mkdir()
    web = build / 'web'
    if web.exists():
        shutil.rmtree(web)
    shutil.copytree(ROOT / 'web/dist', web, ignore=shutil.ignore_patterns('*.map', '*.pdb', '*.d.ts', 'bundle-report.json'))
    revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    engine_source = args.engine_source
    suffix = '.exe' if args.os == 'win' else ''
    native = engine / ('nonplanar_slice_tool' + suffix)
    shutil.copy2(tool, native)
    native.chmod(0o755)
    prefix = args.dependency_prefix or ROOT / 'engine/deps/build/OrcaSlicer_dep/usr/local'
    if args.os == 'mac':
        mac_libraries(native, prefix, engine)
    elif args.os == 'win':
        windows_libraries(native, prefix, engine)
    else:
        if sys.platform != 'linux':
            raise RuntimeError('Linux payloads must be staged on Linux')
        run('strip', '--strip-unneeded', native)
    resources = engine / 'resources'
    for name in ['profiles', 'nonplanar', 'info', 'printers', 'filament_mixing', 'fonts', 'handy_models', 'calib', 'cert', 'flush']:
        shutil.copytree(ROOT / 'engine/resources' / name, resources / name)
    run(sys.executable, ROOT / 'desktop/scripts/compact-profiles.py', resources)
    shutil.copy2(ROOT / 'LICENSE', web / 'licence.txt')
    run(sys.executable, ROOT / 'desktop/scripts/bundle-native-notices.py', '--root', ROOT, '--web', web, '--prefix', prefix)
    duplicate_bytes = 0
    for asset in (web / 'profiles').rglob('*'):
        if not asset.is_file() or asset.suffix.lower() not in {'.png', '.stl'}:
            continue
        original = resources / 'profiles' / asset.relative_to(web / 'profiles')
        if not original.is_file() or digest(asset) != digest(original):
            raise RuntimeError(f'Profile asset has no identical shared resource: {asset}')
        duplicate_bytes += asset.stat().st_size
        asset.unlink()
    print(f'Removed {duplicate_bytes} duplicated profile-asset bytes; served from bundled resources')
    server = engine / ('orca-webapp-server' + suffix)
    env = {**os.environ, 'CGO_ENABLED': '0', 'GOARCH': {'x64': 'amd64', 'arm64': 'arm64'}[args.arch]}
    run('go', 'build', '-trimpath', '-mod=vendor', '-ldflags',
        f'-s -w -X main.buildRevision={revision} -X main.buildImage=pancakeslicer-desktop-{args.os}-{args.arch}',
        '-o', server, '.', cwd=ROOT / 'server', env=env)
    server.chmod(0o755)
    shutil.copy2(ROOT / 'server/schemas.json', engine / 'schemas.json')
    if args.os == 'mac':
        run('codesign', '--force', '--sign', '-', server)
    payload_modes(engine, {native, server})
    payload_modes(web, set())
    if args.os == 'linux' and args.arch == 'arm64':
        print(f'Staged {revision}: {len(list(resources.rglob("*")))} resources for {args.os}/{args.arch}')
        return
    resource_hash, count = resource_identity(resources)
    qualification = build / f'qualification-{args.os}-{args.arch}'
    if qualification.exists():
        shutil.rmtree(qualification)
    qualification.mkdir(parents=True)
    reference = args.reference.resolve()
    reference.mkdir(parents=True, exist_ok=True)
    command_prefix = []
    image = args.image or f'pancakeslicer:{revision[:10]}'
    if args.os == 'linux' and args.arch == 'x64':
        (build / '.dockerignore').write_text('**\n!engine-linux-x64/\n!engine-linux-x64/**\n!web/\n!web/**\n')
        run('docker', 'build', '--file', ROOT / 'server/Dockerfile', '--tag', image, build)
        command_prefix = ['docker', 'run', '--rm', '--network', 'none', '--memory', '2g', '--memory-swap', '2g', '--cpus', '1', '--user', f'{os.getuid()}:{os.getgid()}',
                          '--mount', f'type=bind,src={engine},dst={engine},readonly',
                          '--mount', f'type=bind,src={qualification},dst={qualification}',
                          '--mount', f'type=bind,src={reference},dst={reference}',
                          '--entrypoint', '', image]
    if not (reference / 'qualification.json').is_file():
        if args.os != 'linux' or args.arch != 'x64':
            raise RuntimeError('A Linux x86_64 qualification reference is required for this target')
        run(sys.executable, ROOT / 'engine/scripts/qualification/qualify.py', '--command-prefix', json.dumps(command_prefix), '--tool', native,
            '--resources', resources, '--engine-source', engine_source, '--output', reference)
        if not json.loads((reference / 'qualification.json').read_text())['audits_passed']:
            raise RuntimeError('The engine reference failed its audits')
    run(sys.executable, ROOT / 'engine/scripts/qualification/qualify.py', '--command-prefix', json.dumps(command_prefix), '--tool', native,
        '--resources', resources, '--engine-source', engine_source, '--publication-source', revision,
        '--reference', reference, '--output', qualification)
    run(sys.executable, ROOT / 'engine/scripts/qualification/finalize.py', '--engine', engine,
        '--qualification', qualification, '--evidence-archive', build / f'PancakeSlicer-{args.os}-{args.arch}-qualification.zip')
    components = {'client_source': revision, 'server_source': revision, 'native_source': engine_source,
                  'native_binary_sha256': digest(native), 'native_resources_sha256': resource_hash,
                  'server_binary_sha256': digest(server),
                  'native_manifest_sha256': digest(engine / 'surface-manifest.json'),
                  'native_qualification_sha256': digest(engine / 'artifact-qualification.ready')}
    write_json(engine / 'COMPONENTS.json', components)
    write_json(web / 'COMPONENTS.json', components)
    (web / 'REVISION.txt').write_text(revision + '\n', encoding='utf-8')
    payload_modes(engine, {native, server})
    payload_modes(web, set())
    if args.os == 'linux' and args.arch == 'x64':
        run('docker', 'build', '--file', ROOT / 'server/Dockerfile', '--tag', image, build)
        print(f'Self-hosted image: {image}')
        return
    if args.os == 'mac':
        run('git', 'archive', '--format=tar.gz', '--output=' + str(build / 'source.tar.gz'), 'HEAD', cwd=ROOT)
    png = (ROOT / 'desktop/icon-256.png').read_bytes()
    large = (ROOT / 'desktop/icon-1024.png').read_bytes()
    chunks = b''.join(kind + struct.pack('>I', len(data) + 8) + data for kind, data in ((b'ic10', large), (b'ic08', png)))
    (build / 'icon.icns').write_bytes(b'icns' + struct.pack('>I', len(chunks) + 8) + chunks)
    (build / 'icon.ico').write_bytes(struct.pack('<HHH', 0, 1, 1) +
        struct.pack('<BBBBHHII', 0, 0, 0, 0, 1, 32, len(png), 22) + png)
    print(f'Staged {revision}: {count} resources for {args.os}/{args.arch}')


if __name__ == '__main__':
    main()
