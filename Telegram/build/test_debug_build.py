import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    'debug_build', Path(__file__).with_name('debug_build.py'))
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class DebugBuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.out = self.root / 'out'
        self.out.mkdir()

    def file(self, relative, content=b'keep'):
        path = self.out / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def descriptions(self):
        for index in range(4):
            folder = self.out / 'build/XCBuildData' / (
                format(index, '032x') + '.xcbuilddata')
            folder.mkdir(parents=True)
            (folder / 'manifest.json').write_text('{}')
            (folder / 'build-request.json').write_text('{}')
            os.utime(folder, (100 + index, 100 + index))

    def test_prune_bounds_history_and_preserves_debugging_inputs(self):
        self.descriptions()
        temporary = self.file('build/Telegram.build/Debug/Objects-normal/arm64/chat-1234abcd.o.tmp')
        archive = self.file('build/td_ui.build/Debug/libtd_ui.a.Abc123')
        protected = [self.file(name) for name in (
            'build/Telegram.build/Debug/Objects-normal/arm64/chat.o',
            'build/td_ui.build/Debug/libtd_ui.a',
            'build/SharedPrecompiledHeaders/current.gch',
            'build/XCBuildData/build.db',
            'build/Telegram.build/Release/old-1234abcd.o.tmp',
            'Debug/Telegram.app/Contents/MacOS/Telegram',
            'Debug/TelegramForcePortable/tdata/key_data',
            'zed-profile/tdata/key_data',
        )]
        build.prune(self.out)
        self.assertFalse(temporary.exists())
        self.assertFalse(archive.exists())
        history = sorted((self.out / 'build/XCBuildData').glob('*.xcbuilddata'))
        self.assertEqual([path.name for path in history], [
            format(index, '032x') + '.xcbuilddata' for index in (2, 3)])
        self.assertTrue(all(path.read_bytes() == b'keep' for path in protected))
        self.assertEqual(build.candidates(self.out), [])

    def test_preview_does_not_delete(self):
        path = self.file('build/td_ui.build/Debug/libtd_ui.a.Abc123')
        build.prune(self.out, dry_run=True)
        self.assertTrue(path.exists())

    def test_symlinked_subtrees_and_unrecognized_files_are_untouched(self):
        outside = self.root / 'outside'
        outside.mkdir()
        file = outside / 'chat-1234abcd.o.tmp'
        file.write_bytes(b'keep')
        debug = self.out / 'build/Telegram.build/Debug'
        debug.mkdir(parents=True)
        (debug / 'linked').symlink_to(outside, target_is_directory=True)
        ordinary = self.file('build/Telegram.build/Debug/notes.tmp')
        opaque = self.file('build/XCBuildData/custom.xcbuilddata/notes')
        build.prune(self.out)
        self.assertTrue(file.exists())
        self.assertTrue(ordinary.exists())
        self.assertTrue(opaque.exists())

    def test_symlinked_build_root_is_refused(self):
        outside = self.root / 'outside'
        outside.mkdir()
        (self.out / 'build').symlink_to(outside, target_is_directory=True)
        with self.assertRaises(RuntimeError):
            build.candidates(self.out)

    def test_build_lock_cannot_be_acquired_twice(self):
        with build.build_lock(self.out):
            with self.assertRaises(RuntimeError):
                with build.build_lock(self.out):
                    self.fail('second build entered')

    def test_active_compiler_prevents_cleanup(self):
        path = self.file('build/td_ui.build/Debug/libtd_ui.a.Abc123')
        with patch.object(build, 'processes', return_value=[(123, '/usr/bin/clang')]):
            with self.assertRaises(RuntimeError):
                build.prune(self.out)
        self.assertTrue(path.exists())

    def test_running_checkout_app_prevents_build(self):
        exe = self.out / 'Debug/Telegram.app/Contents/MacOS/Telegram'
        with patch.object(build, 'processes', return_value=[(123, str(exe))]):
            build.require_idle(self.out)
            with self.assertRaises(RuntimeError):
                build.require_idle(self.out, building=True)

    def test_foreign_checkout_is_refused(self):
        self.file('CMakeCache.txt', (
            'CMAKE_GENERATOR:INTERNAL=Xcode\n'
            'CMAKE_HOME_DIRECTORY:INTERNAL=/different/checkout\n'
            'CMAKE_COMMAND:INTERNAL=/usr/bin/cmake\n').encode())
        (self.out / 'Telegram.xcodeproj').mkdir()
        with self.assertRaises(RuntimeError):
            build.configuration(self.out, self.root)

    def test_log_rotation_is_bounded(self):
        log = self.out / 'debug-build.log'
        with build.build_log(log, limit=100) as logger:
            for index in range(50):
                logger.info('compile ' + str(index))
        files = list(self.out.glob('debug-build.log*'))
        self.assertLessEqual(len(files), 2)
        self.assertTrue(all(path.stat().st_size <= 100 for path in files))
        self.assertIn('compile 49', log.read_text())

    def test_missing_dependencies_exclude_generated_and_release_inputs(self):
        missing = self.root / 'Libraries/libQt6Core_debug.a'
        release = self.root / 'Libraries/libQt6Core.a'
        generated = self.out / 'build/lib_generated.a'
        project = {'objects': {
            'debug': {'isa': 'XCBuildConfiguration', 'name': 'Debug',
                'buildSettings': {'OTHER_LDFLAGS': [str(missing), str(generated), '-lz']}},
            'release': {'isa': 'XCBuildConfiguration', 'name': 'Release',
                'buildSettings': {'OTHER_LDFLAGS': [str(release)]}},
        }}
        self.assertEqual(build.missing_inputs(project, self.out), [missing])
        missing.parent.mkdir()
        missing.write_bytes(b'archive')
        self.assertEqual(build.missing_inputs(project, self.out), [])

    def test_failed_build_retains_output_and_exit_status(self):
        log = self.out / 'debug-build.log'
        with build.build_log(log) as logger:
            code = build.run_build([sys.executable, '-c',
                'print("link failed"); raise SystemExit(7)'], self.root, logger)
        self.assertEqual(code, 7)
        self.assertIn('link failed', log.read_text())

    def test_cancel_stops_descendant_after_parent_exits(self):
        child = ('import os, signal, time; '
            'signal.signal(signal.SIGTERM, signal.SIG_IGN); '
            'print(os.getpid(), flush=True); time.sleep(60)')
        parent = ('import subprocess, sys; '
            'subprocess.Popen([sys.executable, "-c", %r]).wait()' % child)
        process = subprocess.Popen([sys.executable, '-c', parent],
            stdout=subprocess.PIPE, text=True, start_new_session=True)
        try:
            child_pid = int(process.stdout.readline())
            build.stop_build(process, grace=0.1)
            deadline = time.monotonic() + 2
            while True:
                state = subprocess.run(['ps', '-p', str(child_pid), '-o', 'stat='],
                    text=True, stdout=subprocess.PIPE, check=False).stdout.strip()
                if not state or state.startswith('Z') or time.monotonic() >= deadline:
                    break
                time.sleep(0.05)
            self.assertTrue(not state or state.startswith('Z'), state)
            self.assertIsNotNone(process.returncode)
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
            process.stdout.close()


if __name__ == '__main__':
    unittest.main()
