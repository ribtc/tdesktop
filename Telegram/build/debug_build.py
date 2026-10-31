import argparse
from contextlib import contextmanager
import fcntl
import json
import logging
from logging.handlers import RotatingFileHandler
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time


GIB = 1024 ** 3
BUILD_TOOLS = {'cmake', 'xcodebuild', 'clang', 'clang++', 'libtool', 'swiftc', 'swift-frontend'}
TEMPORARY_FILE = re.compile(r'(?:.+-[0-9a-f]{8}\.o\.tmp|lib.+\.a\.[A-Za-z0-9]{6})')
DESCRIPTION = re.compile(r'[0-9a-f]{32}\.xcbuilddata')


def configuration(out, checkout):
    if out.is_symlink() or (out / 'build').is_symlink():
        raise RuntimeError('构建目录不能是符号链接。')
    cache = out / 'CMakeCache.txt'
    if not cache.is_file() or not (out / 'Telegram.xcodeproj').is_dir():
        raise RuntimeError('未找到已配置的 out/Telegram.xcodeproj。')
    values = {}
    for line in cache.read_text().splitlines():
        if '=' in line and ':' in line.split('=', 1)[0]:
            key, value = line.split('=', 1)
            values[key.split(':', 1)[0]] = value
    if values.get('CMAKE_GENERATOR') != 'Xcode':
        raise RuntimeError('此入口只管理 macOS Xcode Debug 构建。')
    if Path(values.get('CMAKE_HOME_DIRECTORY', '')).resolve() != checkout.resolve():
        raise RuntimeError('CMakeCache 指向其他 checkout，拒绝操作。')
    return values['CMAKE_COMMAND']


def processes():
    result = subprocess.check_output(['ps', '-axo', 'pid=,comm='], text=True)
    return [(int(pid), name.strip()) for pid, name in
        (line.strip().split(None, 1) for line in result.splitlines() if line.strip())]


def missing_inputs(project, out):
    missing = set()
    for entry in project['objects'].values():
        if entry.get('isa') != 'XCBuildConfiguration' or entry.get('name') != 'Debug':
            continue
        flags = entry.get('buildSettings', {}).get('OTHER_LDFLAGS', [])
        if not isinstance(flags, list):
            raise RuntimeError('无法识别 Xcode 的链接输入格式，请检查生成的工程。')
        for flag in flags:
            path = Path(flag)
            if (path.is_absolute() and path.suffix in ('.a', '.o')
                    and '$(' not in flag and out.resolve() not in path.resolve().parents
                    and not path.is_file()):
                missing.add(path)
    return sorted(missing)


def validate_dependencies(out):
    project = json.loads(subprocess.check_output([
        '/usr/bin/plutil', '-convert', 'json', '-o', '-',
        str(out / 'Telegram.xcodeproj/project.pbxproj')], text=True))
    missing = missing_inputs(project, out)
    if missing:
        raise RuntimeError('缺少 %s 个外部链接输入，构建尚未启动：\n%s\n'
            '请恢复对应版本的依赖。共享 Libraries 不属于可自动清理的缓存。'
            % (len(missing), '\n'.join(str(path) for path in missing)))


def require_idle(out, building=False):
    executable = out / 'Debug/Telegram.app/Contents/MacOS/Telegram'
    for pid, name in processes():
        if Path(name).name in BUILD_TOOLS:
            raise RuntimeError('检测到编译工具运行（PID %s，%s），请等待结束后重试。'
                % (pid, Path(name).name))
        if building and Path(name) == executable:
            raise RuntimeError('当前 checkout 的 Telegram 正在运行（PID %s）。'
                '请先关闭它或结束调试；脚本不会终止该进程。' % pid)


@contextmanager
def build_lock(out):
    descriptor = os.open(out / '.debug-build.lock',
        os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, 'w') as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError('这个 checkout 已有构建或清理入口运行。')
        lock.write(str(os.getpid()))
        lock.flush()
        yield


def regular_files(folder):
    if folder.is_symlink() or not folder.is_dir():
        return
    for parent, directories, files in os.walk(folder, followlinks=False):
        directories[:] = [name for name in directories
            if not (Path(parent) / name).is_symlink()
            and name != 'tdata' and not name.endswith('.app')]
        for name in files:
            path = Path(parent) / name
            if not path.is_symlink() and path.is_file():
                yield path


def size(path):
    if path.is_symlink():
        return 0
    if path.is_file():
        return path.stat().st_size
    total = 0
    for parent, directories, files in os.walk(path, followlinks=False):
        directories[:] = [name for name in directories
            if not (Path(parent) / name).is_symlink()]
        for name in files:
            item = Path(parent) / name
            if not item.is_symlink() and item.is_file():
                total += item.stat().st_size
    return total


def candidates(out):
    intermediate = out / 'build'
    if intermediate.is_symlink():
        raise RuntimeError('拒绝清理指向其他位置的 out/build。')
    result = []
    for target in intermediate.glob('*.build'):
        if not target.is_symlink():
            result.extend(path for path in regular_files(target / 'Debug')
                if TEMPORARY_FILE.fullmatch(path.name))
    history = intermediate / 'XCBuildData'
    if not history.is_symlink():
        descriptions = [path for path in history.glob('*.xcbuilddata')
            if DESCRIPTION.fullmatch(path.name) and path.is_dir()
            and not path.is_symlink()
            and all((path / name).is_file() and not (path / name).is_symlink()
                for name in ('manifest.json', 'build-request.json'))]
        descriptions.sort(key=lambda path: (path.stat().st_mtime_ns, path.name), reverse=True)
        result.extend(descriptions[2:])
    return sorted(result)


def prune(out, dry_run=False):
    if not dry_run:
        require_idle(out)
    intermediate = (out / 'build').resolve()
    reclaimed = 0
    for path in candidates(out):
        amount = size(path)
        print('%s %s（%.1f MiB）' % (
            '将清理' if dry_run else '清理', path.relative_to(out), amount / 1024 ** 2), flush=True)
        if not dry_run:
            require_idle(out)
            expected = intermediate / path.relative_to(out / 'build')
            if path.is_symlink() or path.resolve() != expected:
                raise RuntimeError('清理路径已改变，停止操作：' + str(path))
            if path.is_dir():
                shutil.rmtree(path)
            else:
                path.unlink()
        reclaimed += amount
    print('%s %.1f MiB。' % (
        '预计回收' if dry_run else '已回收', reclaimed / 1024 ** 2), flush=True)


def report(out):
    for relative in ('build/Telegram.build', 'build/SharedPrecompiledHeaders',
            'build/XCBuildData', 'build', 'Debug', 'zed-profile'):
        path = out / relative
        if path.exists():
            print('%6.2f GiB  %s' % (size(path) / GIB, path), flush=True)
    print('可用空间：%.2f GiB；上述 build 子目录包含在 build 总量中。'
        % (shutil.disk_usage(out).free / GIB), flush=True)
    prune(out, dry_run=True)


@contextmanager
def build_log(path, limit=32 * 1024 ** 2):
    if path.is_symlink() or Path(str(path) + '.1').is_symlink():
        raise RuntimeError('构建日志不能是符号链接。')
    logger = logging.Logger('debug-build')
    handler = RotatingFileHandler(path, maxBytes=limit, backupCount=1, encoding='utf-8')
    handler.setFormatter(logging.Formatter('%(message)s'))
    logger.addHandler(handler)
    try:
        yield logger
    finally:
        handler.close()


def interrupted(signum, frame):
    raise KeyboardInterrupt()


def stop_build(process, grace=10):
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        process.wait()
        return
    deadline = time.monotonic() + grace
    while time.monotonic() < deadline:
        process.poll()
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            process.wait()
            return
        time.sleep(0.05)
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait(timeout=5)


def run_build(command, checkout, logger):
    environment = os.environ.copy()
    environment.pop('QT', None)
    logger.info('开始 Debug 构建：%s', ' '.join(command))
    process = subprocess.Popen(command, cwd=checkout, env=environment,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        errors='replace', start_new_session=True)
    previous = signal.signal(signal.SIGTERM, interrupted)
    previous_interrupt = signal.getsignal(signal.SIGINT)
    try:
        for line in process.stdout:
            logger.info(line.rstrip('\n'))
            print(line, end='', flush=True)
        return process.wait()
    except (KeyboardInterrupt, BrokenPipeError):
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        stop_build(process)
        return 130
    finally:
        process.stdout.close()
        signal.signal(signal.SIGTERM, previous)
        signal.signal(signal.SIGINT, previous_interrupt)


def main():
    parser = argparse.ArgumentParser(description='macOS Debug 增量构建与有边界的残留清理。')
    parser.add_argument('action', nargs='?', choices=('build', 'report', 'prune'), default='build')
    parser.add_argument('--dry-run', action='store_true', help='只预览 prune，不删除')
    parser.add_argument('--min-free-gib', type=float, default=8, help='构建前最低可用空间，默认 8 GiB')
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 2, 8))
    args = parser.parse_args()
    if args.jobs < 1 or not math.isfinite(args.min_free_gib) or args.min_free_gib < 0:
        parser.error('jobs 必须为正整数，min-free-gib 不能为负。')
    if args.dry_run and args.action != 'prune':
        parser.error('--dry-run 仅用于 prune；report 本身只读。')
    if sys.platform != 'darwin':
        parser.error('此脚本只支持 macOS；其他平台继续使用原有构建入口。')
    checkout = Path(__file__).resolve().parents[2]
    out = checkout / 'out'
    cmake = configuration(out, checkout)
    if args.action == 'report':
        report(out)
        return 0
    if args.dry_run:
        prune(out, dry_run=True)
        return 0
    with build_lock(out):
        require_idle(out, building=args.action == 'build')
        if args.action == 'build':
            validate_dependencies(out)
        prune(out)
        if args.action == 'prune':
            return 0
        free = shutil.disk_usage(out).free / GIB
        if free < args.min_free_gib:
            raise RuntimeError('可用空间 %.2f GiB，低于 %.2f GiB，已停止构建。'
                '运行 report 查看占用；不会自动删除对象文件或账号数据。'
                % (free, args.min_free_gib))
        command = [cmake, '--build', str(out), '--config', 'Debug',
            '--target', 'Telegram', '--parallel', str(args.jobs)]
        with build_log(out / 'debug-build.log') as logger:
            result = run_build(command, checkout, logger)
            logger.info('Debug 构建退出码：%s', result)
        if result == 0:
            prune(out)
        print('构建日志：%s（最多两份，每份 32 MiB）' % (out / 'debug-build.log'), flush=True)
        return result


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        print('错误：' + str(error), file=sys.stderr)
        sys.exit(1)
