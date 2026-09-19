"""Offline orchestration tests: tiny fake dependency installers, no downloads/builds."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
POWERSHELL = shutil.which('pwsh') or shutil.which('powershell')
CMAKE = os.environ.get('CMAKE_COMMAND') or shutil.which('cmake')


@unittest.skipUnless(os.name == 'nt' and POWERSHELL, 'Windows PowerShell required')
class NativeSetupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='screenshare setup ')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.scripts = self.root / 'scripts'
        self.scripts.mkdir()
        pins = self.root / 'cmake/dependencies'
        pins.mkdir(parents=True)
        for name in ('native-dependencies.json', 'webrtc-source.json', 'webrtc-build.patch'):
            shutil.copyfile(ROOT / 'cmake/dependencies' / name, pins / name)
        shutil.copyfile(ROOT / 'scripts/prepare-dependencies.ps1', self.scripts / 'prepare-dependencies.ps1')
        shutil.copyfile(ROOT / 'cmake/PrepareNativeBuild.cmake', self.root / 'cmake/PrepareNativeBuild.cmake')
        self.cache = self.root / '.deps'
        self.source = self.cache / 'webrtc/checkout/src'
        self.env = os.environ.copy()
        # Only the revision read in readiness checks is stubbed, per subprocess.
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        native = json.loads((pins / 'native-dependencies.json').read_text())
        (self.bin / 'git.cmd').write_text('@echo off\necho ' + native['vulkanHeadersCommit'] + '\nexit /b 0\n')
        self.env['PATH'] = str(self.bin) + os.pathsep + self.env['PATH']
        (self.scripts / 'install-native-deps.ps1').write_text(r'''
param($DependencyRoot)
Add-Content (Join-Path $PSScriptRoot '../calls.txt') native
$lock=Get-Content (Join-Path $PSScriptRoot '../cmake/dependencies/native-dependencies.json') -Raw | ConvertFrom-Json
foreach($component in @('Core','Network','WebSockets','Widgets','Svg')) {
    $file=Join-Path $DependencyRoot "Qt/$($lock.qtVersion)/msvc2022_64/lib/cmake/Qt6$component/Qt6${component}Config.cmake"
    New-Item -ItemType Directory -Force (Split-Path $file) | Out-Null
    Set-Content $file fixture
}
$file=Join-Path $DependencyRoot 'Vulkan-Headers/include/vulkan/vulkan.h'
New-Item -ItemType Directory -Force (Split-Path $file) | Out-Null
Set-Content $file fixture
''', encoding='utf-8')
        (self.scripts / 'sync-webrtc.ps1').write_text(r'''
param($DependencyRoot)
Add-Content (Join-Path $PSScriptRoot '../calls.txt') sync
$file=Join-Path $DependencyRoot 'checkout/src/api/peer_connection_interface.h'
New-Item -ItemType Directory -Force (Split-Path $file) | Out-Null
Set-Content $file fixture
''', encoding='utf-8')
        (self.scripts / 'build-webrtc.ps1').write_text(r'''
param($Configuration,$DependencyRoot,$Jobs)
Add-Content (Join-Path $PSScriptRoot '../calls.txt') "build-$Configuration"
if(Test-Path (Join-Path $PSScriptRoot '../fail-build')){throw 'Deliberate interrupted build'}
$source=Join-Path $DependencyRoot 'checkout/src'
$artifact=Join-Path $source "out/screenshare-$($Configuration.ToLowerInvariant())"
foreach($file in @('third_party/ninja/ninja.exe','third_party/llvm-build/Release+Asserts/bin/clang-cl.exe','third_party/llvm-build/Release+Asserts/bin/lld-link.exe',"out/screenshare-$($Configuration.ToLowerInvariant())/obj/webrtc.lib","out/screenshare-$($Configuration.ToLowerInvariant())/args.gn")) {
    $path=Join-Path $source $file
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    Set-Content $path fixture
}
$lock=Get-Content (Join-Path $PSScriptRoot '../cmake/dependencies/webrtc-source.json') -Raw | ConvertFrom-Json
@{
 schemaVersion=1;sourceRevision=$lock.commit;configuration=$Configuration;architecture='x64';sourceDirectory=$source
 buildPatchSha256=(Get-FileHash (Join-Path $PSScriptRoot '../cmake/dependencies/webrtc-build.patch')).Hash
 compilerSha256=(Get-FileHash (Join-Path $source 'third_party/llvm-build/Release+Asserts/bin/clang-cl.exe')).Hash
 gnArgumentsSha256=(Get-FileHash (Join-Path $artifact 'args.gn')).Hash
 librarySha256=(Get-FileHash (Join-Path $artifact 'obj/webrtc.lib')).Hash
} | ConvertTo-Json | Set-Content (Join-Path $artifact 'screenshare-artifact.json')
''', encoding='utf-8')

    def run_setup(self, *args, success=True):
        result = subprocess.run([POWERSHELL, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                                 str(self.scripts / 'prepare-dependencies.ps1'), *args],
                                env=self.env, text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def calls(self):
        path = self.root / 'calls.txt'
        return path.read_text(encoding='utf-8-sig').splitlines() if path.exists() else []

    def test_check_only_does_not_download_or_create_cache(self):
        result = self.run_setup('-CheckOnly', success=False)
        self.assertIn('missing or stale', result.stderr)
        self.assertFalse(self.cache.exists())
        self.assertEqual(self.calls(), [])

    def test_clean_setup_reuses_cache_after_build_deletion(self):
        self.run_setup()
        self.assertEqual(self.calls(), ['native', 'sync', 'build-Release'])
        output = self.root / 'build/release'
        output.mkdir(parents=True)
        shutil.rmtree(self.root / 'build')
        self.run_setup()
        self.run_setup('-CheckOnly')
        self.assertEqual(self.calls(), ['native', 'sync', 'build-Release'])

    def test_debug_reuses_sources_and_qt(self):
        self.run_setup()
        self.run_setup('-Configuration', 'Debug')
        self.assertEqual(self.calls(), ['native', 'sync', 'build-Release', 'build-Debug'])

    def test_interrupted_build_is_retried(self):
        (self.root / 'fail-build').touch()
        self.run_setup(success=False)
        (self.root / 'fail-build').unlink()
        self.run_setup()
        self.assertEqual(self.calls(), ['native', 'sync', 'build-Release', 'build-Release'])

    def test_changed_patch_rebuilds_without_redownloading(self):
        self.run_setup()
        with (self.root / 'cmake/dependencies/webrtc-build.patch').open('a') as f:
            f.write('\n# fixture patch change\n')
        self.run_setup()
        self.assertEqual(self.calls(), ['native', 'sync', 'build-Release', 'build-Release'])

    def test_custom_cache_path_with_spaces(self):
        alternate = self.root / 'another cache'
        self.run_setup('-DependencyRoot', str(alternate))
        self.run_setup('-DependencyRoot', str(alternate), '-CheckOnly')
        self.assertTrue(alternate.is_dir())
        self.assertFalse(self.cache.exists())

    def test_deleted_library_is_repaired(self):
        self.run_setup()
        (self.source / 'out/screenshare-release/obj/webrtc.lib').unlink()
        self.run_setup()
        self.assertEqual(self.calls(), ['native', 'sync', 'build-Release', 'build-Release'])

    def cmake_prepare(self, settings='', success=True):
        if not CMAKE:
            self.skipTest('CMake required')
        script = self.root / 'prepare.cmake'
        script.write_text('cmake_minimum_required(VERSION 3.24)\nset(CMAKE_BUILD_TYPE Release)\n' + settings + '\ninclude("' +
                          (self.root / 'cmake/PrepareNativeBuild.cmake').as_posix() + '")\n' +
                          'file(WRITE "' + (self.root / 'paths.txt').as_posix() +
                          '" "${CMAKE_CXX_COMPILER}\\n${SCREENSHARE_WEBRTC_ARTIFACT_DIR}\\n${CMAKE_PREFIX_PATH}\\n${Qt6Core_DIR}")\n')
        result = subprocess.run([CMAKE, '-P', str(script)], env=self.env,
                                text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def test_cmake_prepares_missing_compiler_before_project(self):
        self.cmake_prepare('set(SCREENSHARE_BOOTSTRAP_DEPENDENCIES ON)')
        self.assertEqual(self.calls(), ['native', 'sync', 'build-Release'])
        self.assertIn('/.deps/webrtc/', (self.root / 'paths.txt').read_text().replace('\\', '/'))
        self.cmake_prepare('set(SCREENSHARE_BOOTSTRAP_DEPENDENCIES ON)')
        self.assertEqual(self.calls(), ['native', 'sync', 'build-Release'])

    def test_cmake_offline_missing_cache_has_actionable_error(self):
        result = self.cmake_prepare(success=False)
        self.assertIn('prepare-dependencies.ps1', result.stderr)
        self.assertFalse(self.cache.exists())
        self.assertEqual(self.calls(), [])

    def test_cmake_migrates_legacy_cached_defaults(self):
        old = self.root.as_posix() + '/build'
        version = json.loads((self.root / 'cmake/dependencies/native-dependencies.json').read_text())['qtVersion']
        qt = f'Qt/{version}/msvc2022_64'
        paths = {
            'CMAKE_C_COMPILER': 'webrtc/checkout/src/third_party/llvm-build/Release+Asserts/bin/clang-cl.exe',
            'CMAKE_CXX_COMPILER': 'webrtc/checkout/src/third_party/llvm-build/Release+Asserts/bin/clang-cl.exe',
            'CMAKE_LINKER': 'webrtc/checkout/src/third_party/llvm-build/Release+Asserts/bin/lld-link.exe',
            'CMAKE_MAKE_PROGRAM': 'webrtc/checkout/src/third_party/ninja/ninja.exe',
            'SCREENSHARE_WEBRTC_ARTIFACT_DIR': 'webrtc/checkout/src/out/screenshare-release',
            'Vulkan_INCLUDE_DIR': 'Vulkan-Headers/include',
            'Qt6_DIR': qt + '/lib/cmake/Qt6',
            'Qt6Core_DIR': qt + '/lib/cmake/Qt6Core',
        }
        settings = 'set(SCREENSHARE_BOOTSTRAP_DEPENDENCIES ON)\n'
        settings += '\n'.join(f'set({name} "{old}/{path}" CACHE FILEPATH "fixture")' for name, path in paths.items())
        settings += f'\nset(CMAKE_PREFIX_PATH "custom-prefix;{old}/{qt}" CACHE STRING "fixture")'
        self.cmake_prepare(settings)
        output = (self.root / 'paths.txt').read_text().replace('\\', '/')
        self.assertNotIn(old, output)
        self.assertIn('custom-prefix;' + self.cache.as_posix(), output)
        self.assertIn(self.cache.as_posix() + '/' + qt + '/lib/cmake/Qt6Core', output)
        self.assertEqual(self.calls(), ['native', 'sync', 'build-Release'])

    def test_cmake_does_not_replace_incomplete_custom_sdk(self):
        result = self.cmake_prepare('set(SCREENSHARE_BOOTSTRAP_DEPENDENCIES ON)\n'
                                    'set(SCREENSHARE_WEBRTC_ARTIFACT_DIR "missing-custom-sdk")', success=False)
        self.assertIn('Custom native dependency paths are incomplete', result.stderr)
        self.assertFalse(self.cache.exists())
        self.assertEqual(self.calls(), [])

    def test_cmake_reports_preparation_failure(self):
        (self.root / 'fail-build').touch()
        result = self.cmake_prepare('set(SCREENSHARE_BOOTSTRAP_DEPENDENCIES ON)', success=False)
        self.assertIn('preparation failed', result.stderr)

    def test_application_clean_configure_reaches_setup_before_compiler_check(self):
        if not CMAKE:
            self.skipTest('CMake required')
        result = subprocess.run([CMAKE, '--preset', 'release', '-B', str(self.root / 'application output'),
                                 '-DSCREENSHARE_BOOTSTRAP_DEPENDENCIES=OFF',
                                 '-DSCREENSHARE_DEPENDENCY_ROOT=' + str(self.cache)],
                                cwd=ROOT, env=self.env, text=True, capture_output=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('prepare-dependencies.ps1', result.stderr)
        self.assertNotIn('is not a full path to an existing compiler', result.stderr)
        self.assertFalse(self.cache.exists())


if __name__ == '__main__':
    unittest.main()
