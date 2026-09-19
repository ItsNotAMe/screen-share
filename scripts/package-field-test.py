"""Add a concise launcher, silent scene guide and identity manifest to a portable build."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def package(source, destination):
    if destination.exists():
        raise ValueError('Choose a new output ZIP; existing packages are never overwritten')
    with zipfile.ZipFile(source) as archive:
        names = archive.namelist()
        paths = [PurePosixPath(name) for name in names]
        if not paths or any(p.is_absolute() or '..' in p.parts or '\\' in n or ':' in n
                            for p, n in zip(paths, names)):
            raise ValueError('Unsafe portable archive paths')
        roots = {p.parts[0] for p in paths}
        if len(roots) != 1 or len({n.casefold() for n in names}) != len(names):
            raise ValueError('Portable ZIP must have one root and unique Windows paths')
        base = roots.pop()
        required = ['ScreenShare.exe', 'ScreenShareUi.exe', 'ScreenShareFieldScene.exe',
                    'ViGEmClient.dll', 'platforms/qwindows.dll']
        if any(f'{base}/{name}' not in names for name in required):
            raise ValueError('Build package-portable with SCREENSHARE_BUILD_FIELD_TEST=ON first')
        additions = {'Start-V2.cmd': ROOT / 'tools/field-test/Start-V2.cmd',
                     'FIELD-TEST.txt': ROOT / 'tools/field-test/README.txt'}
        if any(f'{base}/{n}'.casefold() in {x.casefold() for x in names}
               for n in [*additions, 'field-test-manifest.json']):
            raise ValueError('Input already contains field-test additions')
        with source.open('rb') as stream:
            portable_hash = hashlib.file_digest(stream, 'sha256').hexdigest()
        manifest = {
            'schema': 1, 'purpose': 'private Stage 2–4 field acceptance',
            'sourceCommit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
            'workingTreeDirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True).strip()),
            'portableSha256': portable_hash,
            'signalingOrigin': 'https://screenshare-signaling-v2.bit-yeet.workers.dev',
            'requiresDeployedV2Service': True, 'installsDrivers': False,
            'externalLatencyVerified': False, 'sha256': {}}
        for name in required:
            manifest['sha256'][name] = hashlib.sha256(archive.read(f'{base}/{name}')).hexdigest()
        for name, path in additions.items():
            manifest['sha256'][name] = hashlib.sha256(path.read_bytes()).hexdigest()
        destination.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(destination, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as output:
            for info in archive.infolist():
                output.writestr(info, archive.read(info))
            for name, path in additions.items():
                output.write(path, f'{base}/{name}')
            output.writestr(f'{base}/field-test-manifest.json', json.dumps(manifest, indent=2))
    with zipfile.ZipFile(destination) as output:
        if output.testzip() is not None:
            raise ValueError('Output ZIP integrity check failed')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('portable', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    print(json.dumps(package(args.portable, args.output), indent=2))
