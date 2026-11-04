import os
import subprocess
import tempfile
import unittest
from pathlib import Path


class ReleaseCleanupTest(unittest.TestCase):
    def test_preserves_sources_needed_by_application_build(self):
        workflow = Path(__file__).resolve().parents[2] / '.github/workflows/ci.yml'
        commands = [line.strip().removeprefix('run: ')
            for line in workflow.read_text().splitlines()
            if line.strip().startswith('run: find $LibrariesPath ')]
        self.assertEqual(len(commands), 1)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            keep = [
                'tde2e/sqlite/sqlite/sqlite3.c',
                'tde2e/sqlite/sqlite/sqlite3.h',
                'tde2e/out/Release/tdsqlite.lib',
                'cache_keys/tde2e',
                'Qt-6.11.2/include/QtCore/qglobal.h',
            ]
            remove = ['tde2e/td/unused.cpp', 'Qt-6.11.2/temp.obj']
            for name in keep + remove:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('fixture')
            subprocess.run(['bash', '-e', '-c', commands[0]], check=True,
                env={**os.environ, 'LibrariesPath': directory})
            for name in keep:
                self.assertTrue((root / name).is_file(), name)
            for name in remove:
                self.assertFalse((root / name).exists(), name)


if __name__ == '__main__':
    unittest.main()
