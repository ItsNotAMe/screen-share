"""Export/verify a content-addressed, offline WebRTC SDK. No network access."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()


def checked_path(root, name):
    path = (root / name).resolve()
    if not path.is_relative_to(root.resolve()) or path == root.resolve():
        raise ValueError(f'Invalid SDK relative path: {name}')
    return path


def verify(root):
    root = root.resolve()
    metadata = json.loads((root / 'screenshare-artifact.json').read_text(encoding='utf-8-sig'))
    if metadata['schemaVersion'] != 2:
        raise ValueError('Expected exported SDK schema 2')
    inventory = metadata['files']
    actual_files = {path.relative_to(root).as_posix() for path in root.rglob('*')
                    if path.is_file() and path != root / 'screenshare-artifact.json'}
    if actual_files != set(inventory):
        raise ValueError('SDK files differ from the inventory (missing or extra files)')
    for name, expected in inventory.items():
        if digest(checked_path(root, name)) != expected:
            raise ValueError(f'SDK checksum mismatch: {name}')
    identity = dict(metadata)
    expected = identity.pop('cacheIdentity')
    if hashlib.sha256(canonical(identity)).hexdigest() != expected:
        raise ValueError('SDK cache identity mismatch')
    for required in ('obj/webrtc.lib', 'args.gn', 'include/api/peer_connection_interface.h', 'notices/LICENSE.md'):
        if required not in inventory:
            raise ValueError(f'SDK inventory missing {required}')
    for library in metadata.get('sdkLibraries', []):
        checked_path(root, library)
        if library not in inventory:
            raise ValueError(f'SDK library is not inventoried: {library}')
    print(f'Verified SDK {expected}: {len(inventory)} files')
    return metadata


def export(artifact, cache, resume=None):
    artifact = artifact.resolve()
    metadata = json.loads((artifact / 'screenshare-artifact.json').read_text(encoding='utf-8-sig'))
    if metadata['schemaVersion'] != 1:
        raise ValueError('Export requires an original dependency build artifact')
    for field in ('msvcToolset', 'windowsSdk'):
        if not metadata.get(field):
            raise ValueError(f'Missing {field}; rerun build-webrtc.ps1 -SkipHooks')
    source = Path(metadata.pop('sourceDirectory')).resolve()
    if cache.resolve().is_relative_to(source):
        raise ValueError('SDK cache must be outside the source checkout')
    for name, field in [('obj/webrtc.lib', 'librarySha256'), ('args.gn', 'gnArgumentsSha256')]:
        if digest(artifact / name) != metadata[field]:
            raise ValueError(f'Build artifact changed: {name}')
    compiler = source / 'third_party/llvm-build/Release+Asserts/bin/clang-cl.exe'
    if digest(compiler) != metadata['compilerSha256']:
        raise ValueError('Compiler differs from the dependency build')
    # No overwrites or recursive deletion. Interrupted exports remain inspectable.
    cache.mkdir(parents=True, exist_ok=True)
    # This is a persistent SDK shared with the developer's build tools, not a
    # private temporary directory. Python's mkdtemp uses owner-only Windows ACLs.
    staging = checked_path(cache, resume) if resume else cache / ('export-' + uuid.uuid4().hex)
    if not resume:
        staging.mkdir()  # Inherit the dependency cache's normal permissions.
    if not staging.is_dir() or not staging.name.startswith('export-'):
        raise ValueError('Resume requires an export staging directory inside the cache')
    def copy(path, name):
        target = checked_path(staging, name)
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or digest(path) != digest(target):
            shutil.copyfile(path, target)
    copy(artifact / 'obj/webrtc.lib', 'obj/webrtc.lib')
    copy(artifact / 'args.gn', 'args.gn')
    # Preserve the header tree, including transitive private includes. Generated
    # headers have a separate root; compiler/SDK/system headers remain external.
    prune = {'.git', '.cache', 'out', 'node_modules', 'llvm-build', '__pycache__'}
    extensions = {'.h', '.hpp', '.hxx', '.inc', '.inl', '.def'}
    for base, directories, files in os.walk(source):
        directories[:] = [name for name in directories if name not in prune and not (Path(base) / name).is_symlink()]
        for name in files:
            path = Path(base) / name
            if path.suffix.lower() in extensions and not path.is_symlink():
                copy(path, 'include/' + path.relative_to(source).as_posix())
    for path in (artifact / 'gen').rglob('*'):
        if path.is_file() and path.suffix.lower() in extensions:
            copy(path, 'gen/' + path.relative_to(artifact / 'gen').as_posix())
    # Use the pinned project's own GN dependency-to-license mapping. Unknown
    # dependencies fail rather than silently omitting their notices.
    notices = staging / 'notices'
    notices.mkdir(exist_ok=True)
    environment = dict(os.environ, DEPOT_TOOLS_WIN_TOOLCHAIN='0', DEPOT_TOOLS_UPDATE='0')
    environment['PATH'] = str(source.parent.parent / 'depot_tools') + os.pathsep + environment.get('PATH', '')
    vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    environment['GYP_MSVS_OVERRIDE_PATH'] = subprocess.check_output([str(vswhere), '-latest', '-products', '*',
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    subprocess.run([sys.executable, str(Path(__file__).with_name('generate-webrtc-notices.py').resolve()),
                    str(source), str(artifact), str(notices)], cwd=source, env=environment, check=True)
    for name in ('LICENSE', 'PATENTS', 'AUTHORS'):
        copy(source / name, 'notices/' + name)
    system_libraries = []
    metadata['sdkLibraries'] = []
    for library in metadata['systemLibraries']:
        if '/' in library or '\\' in library or ':' in library:
            dependency = Path(library).resolve()
            if not dependency.is_relative_to(source) or dependency.suffix != '.lib':
                raise ValueError(f'Unsupported external library: {library}')
            name = 'lib/' + dependency.relative_to(source).as_posix()
            copy(dependency, name)
            metadata['sdkLibraries'].append(name)
        else:
            system_libraries.append(library)
    metadata['systemLibraries'] = system_libraries
    copy(source / 'third_party/compiler-rt/src/LICENSE.TXT', 'notices/compiler-rt-LICENSE.txt')
    metadata['schemaVersion'] = 2
    metadata['files'] = {path.relative_to(staging).as_posix(): digest(path)
                         for path in sorted(staging.rglob('*'))
                         if path.is_file() and path != staging / 'screenshare-artifact.json'}
    metadata['cacheIdentity'] = hashlib.sha256(canonical(metadata)).hexdigest()
    (staging / 'screenshare-artifact.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    verify(staging)
    destination = cache / metadata['cacheIdentity']
    if destination.exists():
        verify(destination)
        print(f'Identical SDK already exists; duplicate staging preserved: {staging}')
    else:
        staging.rename(destination)
    print(destination)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    create = sub.add_parser('export')
    create.add_argument('artifact', type=Path)
    create.add_argument('cache', type=Path)
    create.add_argument('--resume', help='Relative export staging directory left by an interrupted export')
    check = sub.add_parser('verify')
    check.add_argument('sdk', type=Path)
    args = parser.parse_args()
    try:
        if args.action == 'export':
            export(args.artifact, args.cache.resolve(), args.resume)
        else:
            verify(args.sdk)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')
