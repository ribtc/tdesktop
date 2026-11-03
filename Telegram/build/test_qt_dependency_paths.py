import os
import subprocess
import tempfile
import unittest
from pathlib import Path


class QtDependencyPathsTest(unittest.TestCase):
    def test_windows_dependency_paths(self):
        repository = Path(__file__).resolve().parents[2]
        for architecture, win64, winarm in [('x86', 0, 0), ('x64', 1, 0), ('ARM64', 0, 1)]:
            with self.subTest(architecture=architecture), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                libraries = root / 'Libraries'
                names = ['zlib/Debug/libzsd.lib', 'zlib/Release/libzs.lib']
                names += [f'libwebp/out/release-static/{architecture}/lib/{name}.lib'
                    for name in ('webp', 'webpdemux', 'webpmux')]
                for name in names:
                    path = libraries / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.touch()
                (root / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.25)
project(DependencyPaths NONE)
set(WIN32 TRUE)
set(APPLE FALSE)
set(LINUX FALSE)
set(build_win64 {win64})
set(build_winarm {winarm})
set(qt_requested 6.11.2)
set(QT_VERSION 6.11.2)
set(QT_VERSION_MAJOR 6)
set(libs_loc "{libraries.as_posix()}")
macro(find_package)
endmacro()
include("{repository.as_posix()}/cmake/external/qt/package.cmake")
foreach(variable ZLIB_LIBRARY_DEBUG ZLIB_LIBRARY_RELEASE WebP_LIBRARY WebP_demux_LIBRARY WebP_mux_LIBRARY)
    if (NOT EXISTS "${{${{variable}}}}")
        message(SEND_ERROR "${{variable}} points to missing file: ${{${{variable}}}}")
    endif()
endforeach()
''')
                environment = dict(os.environ)
                environment.pop('QT', None)
                result = subprocess.run(['cmake', '-S', directory, '-B', str(root / 'build')],
                    env=environment, text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
