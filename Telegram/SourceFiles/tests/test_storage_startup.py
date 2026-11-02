import argparse
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time


def checksum_file(path, version, data):
    header = b'TDF$' + struct.pack('<I', version)
    digest = hashlib.md5(data + struct.pack('<I', len(data)) + header[4:] + header[:4]).digest()
    path.write_bytes(header + data + digest)


def run(app, profile, sandbox, expected):
    log_path = profile / 'log.txt'
    log_path.unlink(missing_ok=True)
    with (profile / 'stderr.log').open('w') as output:
        process = subprocess.Popen([
            'sandbox-exec', '-f', str(sandbox), str(app),
            '-workdir', str(profile), '-noupdate',
        ], stdout=output, stderr=output)
        try:
            deadline = time.monotonic() + 15
            ready_at = None
            while time.monotonic() < deadline:
                log = log_path.read_text(errors='replace') if log_path.exists() else ''
                if expected is not None and expected in log:
                    ready_at = ready_at or time.monotonic()
                    if time.monotonic() - ready_at > 2:
                        return
                if expected is None:
                    assert 'FATAL:' not in log, f'{profile.name}: normal startup failed.'
                    if (profile / 'tdata/key_datas').exists():
                        ready_at = ready_at or time.monotonic()
                        if time.monotonic() - ready_at > 2:
                            return
                if process.poll() is not None:
                    break
                time.sleep(0.1)
            raise AssertionError(f'{profile.name}: startup did not reach {expected!r}')
        finally:
            if process.poll() is None:
                try:
                    subprocess.run([
                        'sandbox-exec', '-f', str(sandbox), str(app),
                        '-workdir', str(profile), '-quit', '-noupdate',
                    ], stdout=output, stderr=output, timeout=10, check=True)
                    assert process.wait(timeout=10) == 0, 'The app did not exit cleanly.'
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('executable', type=Path)
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('This offline app-level regression runner requires macOS sandbox-exec.')
    app = args.executable.resolve()
    with tempfile.TemporaryDirectory(prefix='ultra-storage-regression-') as temporary:
        root = Path(temporary)
        sandbox = root / 'offline.sb'
        sandbox.write_text('(version 1)\n(allow default)\n(deny network*)\n'
                           '(allow network* (local unix-socket) (remote unix-socket))\n')
        baseline = root / 'baseline'
        baseline.mkdir()
        run(app, baseline, sandbox, None)
        key = baseline / 'tdata/key_datas'
        assert key.exists(), 'An empty directory must initialize normally.'
        original = key.read_bytes()
        run(app, baseline, sandbox, None)
        assert key.read_bytes() == original, 'A normal restart must preserve the local key.'
        print('PASS empty directory and normal restart')
        for case in ('corrupt-key', 'future-key', 'bad-info', 'bad-wrap', 'missing-key',
                     'bad-map', 'bad-authorization'):
            profile = root / case
            shutil.copytree(baseline, profile)
            data = profile / 'tdata'
            key = data / 'key_datas'
            if case == 'corrupt-key':
                key.write_bytes(b'corrupt-key-must-not-be-replaced')
            elif case == 'future-key':
                raw = key.read_bytes()
                checksum_file(key, 999999999, raw[8:-16])
            elif case in ('bad-info', 'bad-wrap'):
                raw = key.read_bytes()
                content = bytearray(raw[8:-16])
                offset = 0
                for _ in range(2 if case == 'bad-info' else 1):
                    offset += 4 + struct.unpack_from('>I', content, offset)[0]
                content[offset + 4] ^= 1
                checksum_file(key, struct.unpack_from('<I', raw, 4)[0], content)
            elif case == 'missing-key':
                key.unlink()
                account = data / 'D877F783D5D3EF8C'
                account.mkdir(exist_ok=True)
                (account / 'maps').write_bytes(b'existing-account-without-key')
            elif case == 'bad-map':
                account = data / 'D877F783D5D3EF8C'
                account.mkdir(exist_ok=True)
                (account / 'maps').write_bytes(b'existing-map-must-not-be-replaced')
            else:
                (data / 'D877F783D5D3EF8Cs').write_bytes(b'authorization-must-not-be-replaced')
            protected = [key, data / 'D877F783D5D3EF8C/maps', data / 'D877F783D5D3EF8Cs']
            before = {p: p.read_bytes() if p.exists() else None for p in protected}
            run(app, profile, sandbox, 'FATAL: Existing account data could not be read.')
            after = {p: p.read_bytes() if p.exists() else None for p in protected}
            assert after == before, f'{case}: account files changed after a read failure.'
            print(f'PASS {case}: account files preserved')


if __name__ == '__main__':
    main()
