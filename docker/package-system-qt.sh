#!/bin/bash
# Публичный native .deb: только Зарница и встроенные шрифты, Qt и runtime берутся из ОС.
# Запускать в zarnitsa-build:astra17 с --network none. Старый package.sh не используется.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8
umask 022

if [[ $# -gt 0 ]]; then
    case "$1" in
        --help|-h)
            [[ $# -eq 1 ]] || { printf '%s\n' 'После --help не нужны другие аргументы.' >&2; exit 2; }
            printf '%s\n' \
                'Использование: bash /src/docker/package-system-qt.sh' \
                'Контейнер: zarnitsa-build:astra17, docker run --network none; без установки на хост.' \
                'Обязательно: ZARNITSA_SOURCE_COMMIT=<полный git SHA>, ZARNITSA_GIT_DIRTY=clean|dirty.' \
                'Каталоги: ZARNITSA_SOURCE_DIR=/src, ZARNITSA_BUILD_DIR=/build/system-qt,' \
                'ZARNITSA_PACKAGE_DIR=/build/package-system-qt, ZARNITSA_DIST_DIR=/src/dist/system-qt.' \
                'Отдельные кандидаты: /build/package-system-qt-<имя> и /src/dist/system-qt-<имя>.' \
                'Дополнительно: ZARNITSA_DEB_VERSION=1.0.0-2, ZARNITSA_BUILD_JOBS=<число>,' \
                'ZARNITSA_BUILD_DATE=<UTC YYYY-MM-DDTHH:MM:SSZ>, SOURCE_DATE_EPOCH=<секунды UTC>,' \
                'ZARNITSA_DOCKER_IMAGE_ID, ZARNITSA_DOCKER_IMAGE_DIGEST, ZARNITSA_BASE_IMAGE_DIGEST.' \
                'Всегда: Release, GUI/tests ON, static helper OFF, native Qt, ctest без pipeline.' \
                'Результат: Zarnitsa_Astra_amd64.deb и SHA256SUMS; существующие файлы/stage не заменяются.'
            exit 0
            ;;
        *) printf 'Неизвестный аргумент: %s\n' "$1" >&2; exit 2 ;;
    esac
fi
if [[ "$(uname -s)" != Linux ]]; then
    printf '%s\n' 'Этот builder запускается только внутри Linux-контейнера zarnitsa-build:astra17.' >&2
    exit 2
fi
export ZARNITSA_SOURCE_DIR="${ZARNITSA_SOURCE_DIR:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)}"

# -I запрещает импорт кода из текущего каталога и PYTHONPATH. Python нужен только сборщику.
python3 -I - <<'PY'
import datetime
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile


ENV = os.environ.copy()
for key in (
        'LD_LIBRARY_PATH', 'LD_PRELOAD', 'LD_AUDIT', 'QT_PLUGIN_PATH',
        'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_CONF_PATH', 'QML2_IMPORT_PATH', 'QML_IMPORT_PATH',
        'QTDIR', 'CMAKE_PREFIX_PATH', 'CMAKE_MODULE_PATH', 'CMAKE_TOOLCHAIN_FILE',
        'CMAKE_GENERATOR', 'PKG_CONFIG_PATH', 'PKG_CONFIG_LIBDIR', 'PKG_CONFIG_SYSROOT_DIR',
        'LIBRARY_PATH', 'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'CPPFLAGS',
        'CFLAGS', 'CXXFLAGS', 'LDFLAGS', 'CC', 'CXX', 'CTEST_PARALLEL_LEVEL'):
    ENV.pop(key, None)
ENV.update(PATH='/usr/sbin:/usr/bin:/sbin:/bin', LANG='C.UTF-8', LC_ALL='C', QT_SELECT='qt5')
ASSET = 'Zarnitsa_Astra_amd64.deb'
ARCH = 'amd64'
APP_VERSION = '1.0.0'
REQUIRED_TESTS = {'test_core', 'test_integrations', 'test_network', 'test_engine',
                  'test_security', 'test_gui', 'test_tools'}
FONT_LICENSES = ('GolosText-OFL.txt', 'JetBrainsMono-OFL.txt')
ICON_SIZES = {16, 22, 24, 32, 48, 64, 96, 128, 256, 512}


def fail(message):
    raise RuntimeError(message)


def run(args, cwd=None, env=None, capture=False):
    command = [str(value) for value in args]
    print('+ ' + ' '.join(shlex.quote(value) for value in command), flush=True)
    # Никакого pipeline: ненулевой ctest/build прерывает упаковку с его кодом возврата.
    result = subprocess.run(command, cwd=str(cwd) if cwd else None, env=env or ENV,
                            check=True, stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.PIPE if capture else None,
                            universal_newlines=True)
    if capture and result.stderr:
        print(result.stderr, end='' if result.stderr.endswith('\n') else '\n', file=sys.stderr, flush=True)
    return result.stdout.strip() if capture else None


def output(*args, **kwargs):
    return run(args, capture=True, **kwargs)


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def inside(path, root):
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def safe_path(value):
    if not value or any(character in value for character in '\r\n\t\x00'):
        fail('Небезопасный путь: {!r}'.format(value))
    path = Path(value)
    if not path.is_absolute() or '..' in path.parts:
        fail('Нужен абсолютный путь без ..: {}'.format(value))
    for part in (path,) + tuple(path.parents):
        if part.is_symlink():
            fail('Символьная ссылка в управляемом пути: {}'.format(part))
    return path.resolve()


def write_text(path, content, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('x', encoding='utf-8', newline='\n') as stream:
        stream.write(content)
    path.chmod(mode)


def plain_copy(source, target, mode=0o644):
    if source.is_symlink() or not source.is_file():
        fail('Ожидался обычный исходный файл: {}'.format(source))
    target.parent.mkdir(parents=True, exist_ok=True)
    # Копируются только байты: без xattrs, ACL, capabilities и setuid/setgid исходника.
    with source.open('rb') as incoming, target.open('xb') as outgoing:
        shutil.copyfileobj(incoming, outgoing)
    target.chmod(mode)


def paths(root):
    result = []
    for current, directories, files in os.walk(str(root), followlinks=False):
        for name in sorted(directories + files):
            result.append(Path(current) / name)
    return sorted(result, key=lambda path: path.relative_to(root).as_posix())


def no_privileges(path, all_xattrs=False):
    if path.is_symlink() or path.lstat().st_mode & 0o6000:
        fail('Ссылка или setuid/setgid недопустимы: {}'.format(path))
    attributes = os.listxattr(str(path), follow_symlinks=False)
    if 'security.capability' in attributes or (all_xattrs and attributes):
        fail('Недопустимые xattrs/capabilities у {}: {}'.format(path, attributes))


def render_template(path, replacements):
    content = path.read_text(encoding='utf-8')
    found = set(re.findall(r'@[A-Za-z_][A-Za-z0-9_]*@', content))
    if found != set(replacements):
        fail('Неверный контракт {}: placeholders {}, ожидались {}'.format(
            path, sorted(found), sorted(replacements)))
    for name, value in replacements.items():
        content = content.replace(name, str(value))
    return content


def main():
    source = safe_path(os.environ['ZARNITSA_SOURCE_DIR'])
    build = safe_path(os.environ.get('ZARNITSA_BUILD_DIR', '/build/system-qt'))
    package = safe_path(os.environ.get('ZARNITSA_PACKAGE_DIR', '/build/package-system-qt'))
    dist = safe_path(os.environ.get('ZARNITSA_DIST_DIR', str(source / 'dist/system-qt')))
    if source == Path('/') or not (source / 'CMakeLists.txt').is_file():
        fail('ZARNITSA_SOURCE_DIR должен указывать на корень исходников Зарницы')
    # Не позволяем опечаткой занять /build/full, чужой staging или произвольный каталог хоста.
    if build.parent != Path('/build') or not re.fullmatch(r'system-qt(?:-[A-Za-z0-9._-]+)?', build.name):
        fail('Build slot должен быть /build/system-qt (либо /build/system-qt-<имя>)')
    if package.parent != Path('/build') or not re.fullmatch(r'package-system-qt(?:-[A-Za-z0-9._-]+)?', package.name):
        fail('Package root должен быть /build/package-system-qt (либо одноимённый отдельный slot)')
    if (dist.parent != source / 'dist' or not re.fullmatch(r'system-qt(?:-[A-Za-z0-9._-]+)?', dist.name)
            or inside(source, Path('/build'))):
        fail('Output разрешён только в <source>/dist/system-qt[-<имя>]; исходники не должны лежать в /build')
    if output('uname', '-m') != 'x86_64' or output('dpkg', '--print-architecture') != ARCH:
        fail('Публичный пакет предназначен только для x86_64/amd64')
    if output('getconf', 'GNU_LIBC_VERSION') != 'glibc 2.28':
        fail('Для публичной Astra-сборки требуется glibc 2.28')
    interfaces = sorted(path.name for path in Path('/sys/class/net').iterdir())
    if interfaces != ['lo']:
        fail('Требуется docker run --network none; обнаружены интерфейсы {}'.format(interfaces))

    commit = os.environ.get('ZARNITSA_SOURCE_COMMIT', '')
    dirty = os.environ.get('ZARNITSA_GIT_DIRTY', '')
    if not re.fullmatch(r'(?:[0-9a-f]{40}|[0-9a-f]{64})', commit) or dirty not in ('clean', 'dirty'):
        fail('Передайте достоверные ZARNITSA_SOURCE_COMMIT (полный SHA) и ZARNITSA_GIT_DIRTY=clean|dirty')
    deb_version = os.environ.get('ZARNITSA_DEB_VERSION', '1.0.0-2')
    if not re.fullmatch(r'1\.0\.0-[1-9][0-9]*(?:[+~.][0-9A-Za-z.+~]*)?', deb_version):
        fail('ZARNITSA_DEB_VERSION должен иметь вид 1.0.0-2[+суффикс]')
    run(['dpkg', '--validate-version', deb_version])
    build_date = os.environ.get('ZARNITSA_BUILD_DATE', datetime.datetime.utcnow().strftime('%Y-%m-%dT%H:%M:%SZ'))
    if not re.fullmatch(r'\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z', build_date):
        fail('ZARNITSA_BUILD_DATE должен быть UTC YYYY-MM-DDTHH:MM:SSZ')
    timestamp = datetime.datetime.strptime(build_date, '%Y-%m-%dT%H:%M:%SZ').replace(tzinfo=datetime.timezone.utc)
    epoch_text = os.environ.get('SOURCE_DATE_EPOCH', str(int(timestamp.timestamp())))
    if not re.fullmatch(r'[0-9]{1,12}', epoch_text):
        fail('SOURCE_DATE_EPOCH должен быть неотрицательным целым числом секунд UTC')
    epoch = int(epoch_text)
    ENV['SOURCE_DATE_EPOCH'] = str(epoch)
    jobs = os.environ.get('ZARNITSA_BUILD_JOBS', str(os.cpu_count() or 1))
    if not re.fullmatch(r'[1-9][0-9]{0,2}', jobs):
        fail('ZARNITSA_BUILD_JOBS должен быть положительным целым числом не более 999')
    image_metadata = {}
    for name, pattern in (
            ('ZARNITSA_DOCKER_IMAGE_ID', r'sha256:[0-9a-f]{64}'),
            ('ZARNITSA_DOCKER_IMAGE_DIGEST', r'(?:[^\s]+@)?sha256:[0-9a-f]{64}'),
            ('ZARNITSA_BASE_IMAGE_DIGEST', r'(?:[^\s]+@)?sha256:[0-9a-f]{64}')):
        value = os.environ.get(name, '')
        if value and not re.fullmatch(pattern, value):
            fail('Некорректное {}'.format(name))
        image_metadata[name] = value or 'NOT_PROVIDED; immutable image identity не проверена'

    def check_git():
        if not shutil.which('git', path=ENV['PATH']):
            return 'передано вызывающей стороной; git отсутствует в build image'
        actual_commit = output('git', '-c', 'safe.directory=' + str(source), '-C', source, 'rev-parse', 'HEAD')
        actual_dirty = bool(output('git', '-c', 'safe.directory=' + str(source), '-C', source,
                                   'status', '--porcelain', '--untracked-files=all'))
        if actual_commit != commit or actual_dirty != (dirty == 'dirty'):
            fail('Переданные SourceCommit/GitDirty расходятся с текущим git-деревом')
        return 'передано вызывающей стороной и сверено с git внутри контейнера'

    git_verification = check_git()
    required_inputs = ('CMakeLists.txt', 'LICENSE', 'docker/package-system-qt.sh',
                       'packaging/zarnitsa.desktop.in', 'packaging/debian-system/control.in',
                       'packaging/debian-system/postinst', 'packaging/debian-system/zarnitsa.sh')
    for name in required_inputs:
        path = source / name
        if path.is_symlink() or not path.is_file():
            fail('Нет обязательного обычного файла {}. Дождитесь готовности native templates.'.format(path))

    def source_fingerprint():
        digest = hashlib.sha256()
        roots = ('CMakeLists.txt', 'src', 'include', 'resources', 'tests', 'cmake', 'translations',
                 'LICENSE', 'packaging/debian-system', 'packaging/zarnitsa.desktop.in',
                 'docker/package-system-qt.sh')
        for name in roots:
            root = source / name
            if not root.exists():
                continue
            for path in [root] + (paths(root) if root.is_dir() else []):
                if path.is_symlink() or not (path.is_dir() or path.is_file()):
                    fail('Ссылка/специальный файл в build inputs: {}'.format(path))
                relative = path.relative_to(source).as_posix()
                if any(character in relative for character in '\n\r\t'):
                    fail('Небезопасное имя исходного файла: {!r}'.format(relative))
                digest.update(relative.encode('utf-8') + b'\0')
                digest.update(sha(path).encode('ascii') if path.is_file() else b'directory')
                digest.update(b'\0')
        return digest.hexdigest()

    source_sha = source_fingerprint()

    def claim_root(root, kind):
        marker = root / '.zarnitsa-system-qt-owner'
        expected = 'ZARNITSA_SYSTEM_QT_BUILDER=1\nKIND={}\nSOURCE={}\n'.format(kind, source)
        root.mkdir(parents=True, exist_ok=True)
        if marker.is_symlink():
            fail('Ссылка вместо ownership marker: {}'.format(marker))
        if marker.exists():
            if not marker.is_file() or marker.read_text(encoding='utf-8') != expected:
                fail('Каталог принадлежит другой задаче; не меняю {}'.format(root))
        elif list(root.iterdir()):
            fail('Непроверенный непустой каталог, не меняю {}. Выделите пустой native slot.'.format(root))
        else:
            write_text(marker, expected)
        lock_path = root / '.system-qt.lock'
        if lock_path.is_symlink() or (lock_path.exists() and not lock_path.is_file()):
            fail('Некорректный lock {}'.format(lock_path))
        lock = lock_path.open('a')
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            fail('Другой native builder уже использует {}'.format(root))
        return lock

    # Держим оба дескриптора до конца процесса, не затрагивая /build/full.
    locks = [claim_root(build, 'build'), claim_root(package, 'package')]
    stage_target = package / 'stage'
    for target in (stage_target, dist / ASSET, dist / 'SHA256SUMS'):
        if target.exists() or target.is_symlink():
            fail('Уже существует {}; сначала отдельно проверьте и отложите предыдущий результат.'.format(target))

    native_records = {}
    package_versions = {}

    def installed_package(name):
        if not re.fullmatch(r'[a-z0-9][a-z0-9+.-]*(?::(?:amd64|all))?', name):
            fail('Некорректное имя native dpkg package: {}'.format(name))
        fields = output('dpkg-query', '-W', '-f=${db:Status-Status}\t${binary:Package}\t${Version}', name).split('\t')
        if len(fields) != 3 or fields[0] != 'installed':
            fail('Пакет не установлен: {}'.format(name))
        package_versions[fields[1]] = fields[2]
        return fields[1].split(':')[0]

    def native_file(path):
        path = Path(path)
        real = path.resolve(strict=True)
        roots = (Path('/lib').resolve(), Path('/lib64').resolve(), Path('/usr/lib').resolve(),
                 Path('/usr/lib64').resolve())
        if not real.is_file() or inside(real, Path('/usr/local')) or not any(inside(real, root) for root in roots):
            fail('Нужен native файл ОС из /lib или /usr/lib, получено {}'.format(real))
        key = str(real)
        if key in native_records:
            return native_records[key]['package']
        candidates = [str(path), str(real)]
        for candidate in list(candidates):
            if candidate.startswith(('/lib/', '/lib64/')):
                candidates.append('/usr' + candidate)
            elif candidate.startswith(('/usr/lib/', '/usr/lib64/')):
                candidates.append(candidate[4:])
        for candidate in dict.fromkeys(candidates):
            result = subprocess.run(['dpkg-query', '-S', candidate], env=ENV,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
            if result.returncode:
                continue
            for line in result.stdout.splitlines():
                if ': ' not in line:
                    continue
                owners, found = line.split(': ', 1)
                if found != candidate:
                    continue
                owner = installed_package(owners.split(', ')[0])
                native_records[key] = {'package': owner, 'path': str(real), 'sha256': sha(real)}
                return owner
        fail('Файл не принадлежит установленному native dpkg package: {}'.format(path))

    qt_version = output('qmake', '-query', 'QT_VERSION')
    if qt_version != '5.15.2':
        fail('Ожидался system Qt 5.15.2 сборочного образа, обнаружен {}'.format(qt_version))
    qt_libs = Path(output('qmake', '-query', 'QT_INSTALL_LIBS')).resolve(strict=True)
    qt_plugins = Path(output('qmake', '-query', 'QT_INSTALL_PLUGINS')).resolve(strict=True)
    qt_cmake = {'Qt5': qt_libs / 'cmake/Qt5'}
    qt_packages = set()
    for component in ('Core', 'Gui', 'Widgets', 'Network', 'Test'):
        library = qt_libs / ('libQt5' + component + '.so.5')
        owner = native_file(library)
        if component != 'Test':
            qt_packages.add(owner)
        qt_cmake['Qt5' + component] = qt_libs / ('cmake/Qt5' + component)
    for name, directory in qt_cmake.items():
        native_file(directory / (name + 'Config.cmake'))
    qpa_provider = native_file(qt_plugins / 'platforms/libqxcb.so')
    # Этот plugin нужен только ctest/генератору значков; в пакет он не копируется.
    native_file(qt_plugins / 'platforms/libqoffscreen.so')
    openssl_providers = set()
    ld_cache = output('/sbin/ldconfig', '-p')
    for soname in ('libssl.so.1.1', 'libcrypto.so.1.1'):
        match = re.search(r'(?m)^\s*' + re.escape(soname) +
                          r'\s+\([^\n]*x86-64[^\n]*\)\s+=>\s+(/\S+)\s*$', ld_cache)
        if not match:
            fail('Нет native {} для QtNetwork dlopen'.format(soname))
        openssl_providers.add(native_file(Path(match.group(1))))
    compiler_version = output('/usr/bin/g++', '-dumpfullversion', '-dumpversion')
    if not compiler_version.startswith('8.3.') or output('/usr/bin/g++', '-dumpmachine') != 'x86_64-linux-gnu':
        fail('Требуется native GCC 8.3 x86_64-linux-gnu из zarnitsa-build:astra17')

    with tempfile.TemporaryDirectory(prefix='.system-qt-', dir=str(package)) as work_name:
        work = Path(work_name)
        stage = work / 'stage'
        stage.mkdir(mode=0o755)
        test_env = ENV.copy()
        for variable, relative in (('HOME', 'home'), ('XDG_CONFIG_HOME', 'home/config'),
                                   ('XDG_DATA_HOME', 'home/data'), ('XDG_CACHE_HOME', 'home/cache'),
                                   ('XDG_RUNTIME_DIR', 'runtime'), ('TMPDIR', 'tmp')):
            directory = work / relative
            directory.mkdir(parents=True, exist_ok=True)
            directory.chmod(0o700)
            test_env[variable] = str(directory)
        test_env['QT_QPA_PLATFORM'] = 'offscreen'

        configure = ['cmake', '-S', source, '-B', build, '-G', 'Ninja',
                     '-DCMAKE_BUILD_TYPE=Release', '-DZARNITSA_GUI=ON', '-DZARNITSA_TESTS=ON',
                     '-DZARNITSA_STATIC_HELPER=OFF', '-DCMAKE_SKIP_RPATH=ON',
                     '-DCMAKE_C_COMPILER=/usr/bin/gcc', '-DCMAKE_CXX_COMPILER=/usr/bin/g++',
                     '-DCMAKE_C_FLAGS=', '-DCMAKE_CXX_FLAGS=', '-DCMAKE_EXE_LINKER_FLAGS=',
                     '-DCMAKE_SHARED_LINKER_FLAGS=', '-DCMAKE_MODULE_LINKER_FLAGS=',
                     '-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG', '-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG']
        configure.extend('-D{}_DIR={}'.format(name, directory) for name, directory in sorted(qt_cmake.items()))
        run(configure)
        cache = {}
        for line in (build / 'CMakeCache.txt').read_text(encoding='utf-8').splitlines():
            match = re.match(r'([^#/:][^:]*):[^=]+=(.*)', line)
            if match:
                cache[match.group(1)] = match.group(2)
        for name, expected in (('CMAKE_PROJECT_VERSION', APP_VERSION), ('CMAKE_BUILD_TYPE', 'Release'),
                               ('ZARNITSA_GUI', 'ON'), ('ZARNITSA_TESTS', 'ON'),
                               ('ZARNITSA_STATIC_HELPER', 'OFF'), ('CMAKE_SKIP_RPATH', 'ON')):
            if cache.get(name) != expected:
                fail('CMake не подтвердил {}={} (получено {!r})'.format(name, expected, cache.get(name)))
        for name, directory in qt_cmake.items():
            if Path(cache.get(name + '_DIR', '')).resolve() != directory:
                fail('CMake использовал не native {} config'.format(name))
        run(['cmake', '--build', build, '--parallel', jobs])
        link_commands = output('ninja', '-C', build, '-t', 'commands', 'zarnitsa', 'zarnitsa-icmp')
        if re.search(r'(^|[\s,])(?:-static(?:-libstdc\+\+|-libgcc)?|-Bstatic)(?=[\s,]|$)', link_commands):
            fail('Обнаружена статическая линковка runtime в native build')
        for token in shlex.split(link_commands):
            if token.startswith('/') and re.search(r'\.so(?:\.[0-9]+)*$', token):
                native_file(Path(token))
            elif token.startswith('/') and token.endswith('.a') and not inside(Path(token).resolve(), build):
                fail('В link command попал чужой static archive: {}'.format(token))

        listed = output('ctest', '--show-only', cwd=build, env=test_env)
        test_names = re.findall(r'(?m)^\s*Test\s+#\d+:\s*(\S+)\s*$', listed)
        if not REQUIRED_TESTS.issubset(test_names) or len(test_names) != len(set(test_names)):
            fail('ctest не зарегистрировал все обязательные наборы: {}'.format(listed))
        run(['ctest', '--output-on-failure', '--parallel', '1'], cwd=build, env=test_env)
        test_log = (build / 'Testing/Temporary/LastTest.log').read_text(encoding='utf-8')
        totals = re.findall(r'Totals:\s+(\d+) passed,\s+(\d+) failed,\s+(\d+) skipped(?:,\s+(\d+) blacklisted)?', test_log)
        if len(totals) != len(test_names) or any(int(row[1]) for row in totals):
            fail('Не удалось подтвердить QtTest totals каждого набора в LastTest.log')
        test_counts = [sum(int(row[index] or 0) for row in totals) for index in range(4)]
        if source_fingerprint() != source_sha:
            fail('Build inputs изменились во время сборки/ctest; повторите после завершения правок')

        def verify_elf(path, helper=False):
            if path.is_symlink() or not path.is_file():
                fail('Нет обычного ELF {}'.format(path))
            no_privileges(path)
            if output('getcap', path):
                fail('ELF уже имеет file capabilities: {}'.format(path))
            header = output('readelf', '--wide', '-h', path)
            if not re.search(r'Class:\s+ELF64\b', header) or not re.search(r'Machine:.*X86-64', header):
                fail('Не ELF64 amd64: {}'.format(path))
            program = output('readelf', '--wide', '-l', path)
            interpreter = re.search(r'Requesting program interpreter:\s*([^\]]+)\]', program)
            if not re.search(r'\bINTERP\b', program) or not interpreter:
                fail('Native executable должен иметь INTERP: {}'.format(path))
            if Path(interpreter.group(1)).name != 'ld-linux-x86-64.so.2':
                fail('Неожиданный ELF interpreter: {}'.format(interpreter.group(1)))
            native_file(Path(interpreter.group(1)))
            dynamic = output('readelf', '--wide', '-d', path)
            if re.search(r'\((?:RPATH|RUNPATH)\)', dynamic):
                fail('RPATH/RUNPATH запрещён: {}'.format(path))
            needed = re.findall(r'\(NEEDED\).*\[([^\]]+)\]', dynamic)
            if not {'libc.so.6', 'libstdc++.so.6', 'libgcc_s.so.1'}.issubset(needed):
                fail('libc/libstdc++/libgcc должны быть динамическими: {}: {}'.format(path, needed))
            if helper and any('qt' in name.lower() for name in needed):
                fail('ICMP helper не должен зависеть от Qt: {}'.format(needed))
            if not helper and not {'libQt5Core.so.5', 'libQt5Gui.so.5', 'libQt5Widgets.so.5',
                                   'libQt5Network.so.5'}.issubset(needed):
                fail('GUI должен использовать native Qt Core/Gui/Widgets/Network: {}'.format(needed))
            versions = output('readelf', '--wide', '--version-info', path)
            glibc = {tuple(int(part) for part in value.split('.'))
                     for value in re.findall(r'\bGLIBC_(\d+(?:\.\d+)+)\b', versions)}
            if not glibc or max(glibc) > (2, 28) or 'GLIBC_PRIVATE' in versions:
                fail('ELF требует ABI новее glibc 2.28 либо PRIVATE: {}'.format(path))
            resolved = output('ldd', '-r', path)
            if re.search(r'not found|undefined symbol', resolved, re.IGNORECASE):
                fail('Не разрешены native зависимости {}:\n{}'.format(path, resolved))
            for line in resolved.splitlines():
                if 'linux-vdso.so.' in line or not line.strip():
                    continue
                match = re.match(r'\s*(?:\S+\s+=>\s+)?(/\S+)\s+\(', line)
                if not match:
                    fail('Неожиданный вывод ldd: {}'.format(line))
                if helper and 'qt' in Path(match.group(1)).name.lower():
                    fail('Qt попал в транзитивные зависимости helper')
                native_file(Path(match.group(1)))
            return {'needed': needed, 'glibc_max': '.'.join(str(value) for value in max(glibc)),
                    'interpreter': interpreter.group(1)}

        original_sha = {}
        elf_info = {}
        packaged_sha = {}
        for name in ('zarnitsa', 'zarnitsa-icmp'):
            original = build / name
            original_sha[name] = sha(original)
            verify_elf(original, helper=name.endswith('-icmp'))
            target = stage / 'usr/lib/zarnitsa' / name
            plain_copy(original, target, 0o755)
            # Никогда не strip build binaries: только свежие копии в текущем task stage.
            run(['strip', '--strip-unneeded', target])
            elf_info[name] = verify_elf(target, helper=name.endswith('-icmp'))
            packaged_sha[name] = sha(target)

        wrapper = source / 'packaging/debian-system/zarnitsa.sh'
        postinst = source / 'packaging/debian-system/postinst'
        for script in (wrapper, postinst):
            content = script.read_bytes()
            if b'\r' in content or not content.startswith((b'#!/bin/bash\n', b'#!/bin/sh\n')):
                fail('Native shell script должен иметь явный shebang и LF: {}'.format(script))
        wrapper_text = render_template(wrapper, {})
        write_text(stage / 'usr/bin/zarnitsa', wrapper_text, 0o755)
        postinst_text = render_template(postinst, {'@HELPER_SHA256@': packaged_sha['zarnitsa-icmp'],
                                                 '@APP_SHA256@': packaged_sha['zarnitsa']})
        write_text(stage / 'DEBIAN/postinst', postinst_text, 0o755)
        for script in (stage / 'usr/bin/zarnitsa', stage / 'DEBIAN/postinst'):
            run(['bash', '-n', script])
        desktop = render_template(source / 'packaging/zarnitsa.desktop.in',
                                  {'@ZARNITSA_EXEC@': '/usr/bin/zarnitsa',
                                   '@ZARNITSA_TRY_EXEC@': '/usr/bin/zarnitsa',
                                   '@ZARNITSA_ROOT@': '/usr/lib/zarnitsa'})
        if re.search(r'%[FUfu]', desktop):
            fail('Неожиданные файловые аргументы .desktop')
        write_text(stage / 'usr/share/applications/zarnitsa.desktop', desktop)
        icon_root = work / 'icons'
        run([build / 'zarnitsa', '--render-icons', icon_root], env=test_env)
        seen_sizes = set()
        icon_files = set()
        for icon in paths(icon_root):
            if icon.is_symlink() or not (icon.is_dir() or icon.is_file()):
                fail('Недопустимый сгенерированный значок: {}'.format(icon))
            if icon.is_dir():
                continue
            relative = icon.relative_to(icon_root).as_posix()
            match = re.fullmatch(r'([0-9]+)x\1/apps/zarnitsa\.png', relative)
            if not match or int(match.group(1)) not in ICON_SIZES:
                fail('Неожиданный путь значка: {}'.format(relative))
            size = int(match.group(1))
            data = icon.read_bytes()
            if len(data) < 24 or data[:8] != b'\x89PNG\r\n\x1a\n' or struct.unpack('>II', data[16:24]) != (size, size):
                fail('Неверный PNG/размер значка {}'.format(icon))
            destination = stage / 'usr/share/icons/hicolor' / relative
            plain_copy(icon, destination)
            icon_files.add(destination.relative_to(stage).as_posix())
            seen_sizes.add(size)
        if seen_sizes != ICON_SIZES:
            fail('Не созданы все штатные значки: {}'.format(sorted(seen_sizes)))

        # dpkg-shlibdeps требует debian/control относительно cwd, а не в source tree.
        shlib_work = work / 'shlibdeps'
        write_text(shlib_work / 'debian/control',
                   'Source: zarnitsa\nSection: net\nPriority: optional\n'
                   'Maintainer: Иванцов Степан Сергеевич <stefanvoskres@gmail.com>\n\n'
                   'Package: zarnitsa\nArchitecture: amd64\nDescription: native system Qt dependency analysis\n')
        shlib_output = output('dpkg-shlibdeps', '-O',
                              '-e' + str(stage / 'usr/lib/zarnitsa/zarnitsa'),
                              '-e' + str(stage / 'usr/lib/zarnitsa/zarnitsa-icmp'), cwd=shlib_work)
        matches = re.findall(r'(?m)^shlibs:Depends=(.+)$', shlib_output)
        if len(matches) != 1:
            fail('Не удалось получить Depends обоих executables: {}'.format(shlib_output))
        raw_depends = matches[0]
        dependencies = []
        dependency_names = set()
        for group in raw_depends.split(','):
            group = group.strip()
            names = re.findall(r'(?:^|\|)\s*([a-z0-9][a-z0-9+.-]*)(?::(?:amd64|any|native))?', group)
            if not names:
                fail('Неожиданная dependency group: {}'.format(group))
            # Astra ci/сборочные ревизии не являются нижним runtime ABI Qt 5.15.
            # libgost не копируется и не получает искусственный минимум 3.5 от builder.
            if any(name in qt_packages or name.startswith('libqt5') for name in names):
                # symbols могут предлагать libqt5gui5 | libqt5gui5-gles: выбираем
                # фактического native provider, как и для обязательного xcb QPA.
                native_qt = [name for name in names if name in qt_packages]
                if len(native_qt) == 1:
                    names = native_qt
                group = ' | '.join(name + ' (>= 5.15)' for name in names)
            elif names == ['libgost']:
                group = 'libgost'
            if group not in dependencies:
                dependencies.append(group)
                dependency_names.update(names)
        explicit = {name: name + ' (>= 5.15)' for name in qt_packages}
        for name in openssl_providers | {qpa_provider} | {
                'ca-certificates', 'iputils-ping', 'libcap2-bin', 'bash', 'coreutils', 'findutils', 'grep', 'sed'}:
            installed_package(name)
            explicit.setdefault(name, name)
        for name, expression in sorted(explicit.items()):
            if name not in dependency_names:
                dependencies.append(expression)
                dependency_names.add(name)
        depends = ', '.join(dependencies)

        doc = stage / 'usr/share/doc/zarnitsa'
        license_text = (source / 'LICENSE').read_text(encoding='utf-8')
        plain_copy(source / 'LICENSE', doc / 'LICENSE')
        for name in FONT_LICENSES:
            license_path = source / 'resources/licenses' / name
            if 'SIL OPEN FONT LICENSE Version 1.1' not in license_path.read_text(encoding='utf-8'):
                fail('Не найден полный OFL встроенного шрифта: {}'.format(name))
            plain_copy(license_path, doc / 'fonts' / name)
        write_text(doc / 'copyright',
                   'Зарница {} — native system-Qt .deb\n\n'.format(deb_version) + license_text +
                   '\nВ executable встроены шрифты Golos Text и JetBrains Mono.\n'
                   'Их copyright и полные SIL OFL 1.1: fonts/GolosText-OFL.txt, fonts/JetBrainsMono-OFL.txt.\n'
                   'Qt, libc, libstdc++, OpenSSL, plugins и системные утилиты в payload НЕ входят.\n'
                   'Это динамические зависимости ОС; их лицензии поставляет дистрибутив.\n')
        native_readme = source / 'packaging/debian-system/README.Debian'
        if native_readme.is_file():
            write_text(doc / 'README.Debian', native_readme.read_text(encoding='utf-8'))
        else:
            write_text(doc / 'README.Debian',
                       'Зарница: публичная native-сборка для Astra Linux 1.7 amd64.\n\n'
                       'Нужны системный Qt >= 5.15 (Core/Gui/Widgets/Network и xcb QPA), OpenSSL 1.1,\n'
                       'CA certificates и зависимости из поля Depends. Это НЕ автономный Qt-less комплект.\n'
                       'Установка/обновление выполняется штатными средствами dpkg/apt из доверенных репозиториев.\n'
                       'Имя пакета zarnitsa сохранено: dpkg сам удаляет прежние файлы этого пакета.\n'
                       'Программа запускается как /usr/bin/zarnitsa; данные пользователя остаются в HOME.\n'
                       'Helper /usr/lib/zarnitsa/zarnitsa-icmp динамический и без Qt; setuid не используется.\n'
                       'postinst пытается настроить cap_net_raw только проверенному helper. Политика МКЦ\n'
                       'и права на конкретной Astra требуют отдельной проверки администратором.\n'
                       'Сборщик не запускает postinst и ничего не устанавливает на хост.\n'
                       'Лицензия/контакты: LICENSE, copyright; OFL встроенных шрифтов: fonts/.\n'
                       'Версии сборки/проверки: BUILD.txt; SHA256SUMS перечисляет файлы payload, кроме себя.\n')

        if source_fingerprint() != source_sha:
            fail('Build/packaging inputs изменились во время упаковки')
        check_git()
        for name, previous_sha in original_sha.items():
            if sha(build / name) != previous_sha:
                fail('Build binary изменился при упаковке: {}'.format(name))
        lines = ['Зарница — протокол native system-Qt сборки (не сертификат релиза)',
                 'PACKAGE=zarnitsa', 'VERSION=' + deb_version, 'APP_VERSION=' + APP_VERSION,
                 'ARCHITECTURE=' + ARCH, 'PACKAGE_MODE=system-qt; no third-party runtime payload',
                 'SOURCE_COMMIT=' + commit, 'GIT_DIRTY=' + dirty, 'GIT_METADATA_SOURCE=' + git_verification,
                 'BUILD_DATE=' + build_date, 'SOURCE_DATE_EPOCH=' + str(epoch),
                 'SOURCE_INPUTS_SHA256=' + source_sha, 'EXPECTED_BUILD_IMAGE=zarnitsa-build:astra17',
                 'NETWORK_INTERFACES=' + ','.join(interfaces),
                 'GLIBC=' + output('getconf', 'GNU_LIBC_VERSION'), 'QT_BUILD_VERSION=' + qt_version,
                 'QT_RUNTIME_MINIMUM=5.15; без Astra ci/revision pin',
                 'CXX_COMPILER=/usr/bin/g++; ' + compiler_version, 'CXX_TARGET=x86_64-linux-gnu',
                 'CMAKE=' + output('cmake', '--version').splitlines()[0],
                 'NINJA=' + output('ninja', '--version'),
                 'BUILD_TYPE=Release; GUI=ON; TESTS=ON; STATIC_HELPER=OFF; CMAKE_SKIP_RPATH=ON',
                 'CTEST=exit 0; --output-on-failure --parallel 1; без pipeline',
                 'CTEST_SUITES=' + ','.join(test_names),
                 'QTTEST_TOTALS=passed {}; failed {}; skipped {}; blacklisted {}'.format(*test_counts),
                 'SHLIBDEPS_RAW=' + raw_depends, 'DEPENDS=' + depends,
                 'STRIP=только копии; SHA build binaries после упаковки неизменны',
                 'PAYLOAD_PRIVILEGES=no setuid/setgid/capabilities/xattrs; maintainer script не выполнялся',
                 'RUNTIME=системные Qt/stdlib/libc/OpenSSL/QPA; чужие .so, plugins и утилиты не копируются',
                 'PRIVILEGED_HOST_VALIDATION=NOT_PERFORMED; МКЦ/установка на живую Astra не проверялись',
                 'REPRODUCIBLE_BUILD=не заявляется; vendor sources/build dependencies не включены',
                 'CERTIFICATION=не заявляется']
        for name, value in sorted(image_metadata.items()):
            lines.append(name + '=' + value)
        for name in ('CMAKE_C_FLAGS', 'CMAKE_C_FLAGS_RELEASE', 'CMAKE_CXX_FLAGS',
                     'CMAKE_CXX_FLAGS_RELEASE', 'CMAKE_EXE_LINKER_FLAGS'):
            lines.append(name + '=' + cache.get(name, ''))
        for os_file in ('/etc/os-release', '/etc/astra_version'):
            if Path(os_file).is_file():
                lines.append('BUILD_OS[{}]={}'.format(os_file, json.dumps(Path(os_file).read_text(encoding='utf-8'), ensure_ascii=False)))
        for name in sorted(original_sha):
            lines.extend(('BUILD_BINARY_SHA256[{}]={}'.format(name, original_sha[name]),
                          'PACKAGED_BINARY_SHA256[{}]={}'.format(name, packaged_sha[name]),
                          'ELF[{}]={}'.format(name, json.dumps(elf_info[name], sort_keys=True))))
        for name, value in sorted(package_versions.items()):
            lines.append('BUILD_NATIVE_PACKAGE[{}]={} (информация, НЕ нижний Depends)'.format(name, value))
        for name, value in sorted(native_records.items()):
            lines.append('BUILD_NATIVE_FILE[{}]={}; {}'.format(name, value['package'], value['sha256']))
        write_text(doc / 'BUILD.txt', '\n'.join(lines) + '\n')
        checksum_lines = []
        for path in paths(stage):
            if path.is_file() and not inside(path, stage / 'DEBIAN'):
                checksum_lines.append('{}  {}\n'.format(sha(path), path.relative_to(stage).as_posix()))
        write_text(doc / 'SHA256SUMS', ''.join(checksum_lines))
        installed_size = sum((path.stat().st_size + 1023) // 1024 for path in paths(stage)
                             if path.is_file() and not inside(path, stage / 'DEBIAN'))
        control = render_template(source / 'packaging/debian-system/control.in',
                                  {'@VERSION@': deb_version, '@ARCH@': ARCH, '@DEPENDS@': depends,
                                   '@INSTALLED_SIZE@': installed_size})
        write_text(stage / 'DEBIAN/control', control)

        allowed_files = {'usr/bin/zarnitsa', 'usr/lib/zarnitsa/zarnitsa', 'usr/lib/zarnitsa/zarnitsa-icmp',
                         'usr/share/applications/zarnitsa.desktop', 'DEBIAN/control', 'DEBIAN/postinst'} | icon_files
        allowed_files.update('usr/share/doc/zarnitsa/' + name for name in
                             ('LICENSE', 'copyright', 'README.Debian', 'BUILD.txt', 'SHA256SUMS'))
        allowed_files.update('usr/share/doc/zarnitsa/fonts/' + name for name in FONT_LICENSES)
        allowed_dirs = {'.'}
        for name in allowed_files:
            allowed_dirs.update(str(parent) for parent in PurePosixPath(name).parents)
        actual_files = set()
        for path in [stage] + paths(stage):
            relative = path.relative_to(stage).as_posix()
            no_privileges(path, all_xattrs=True)
            if path.is_dir():
                if relative not in allowed_dirs:
                    fail('Каталог вне payload whitelist: {}'.format(relative))
                path.chmod(0o755)
            elif path.is_file():
                if relative not in allowed_files:
                    fail('Файл вне payload whitelist: {}'.format(relative))
                expected_mode = 0o755 if relative in ('usr/bin/zarnitsa', 'usr/lib/zarnitsa/zarnitsa',
                                                     'usr/lib/zarnitsa/zarnitsa-icmp', 'DEBIAN/postinst') else 0o644
                if stat.S_IMODE(path.stat().st_mode) != expected_mode:
                    fail('Неожиданный mode {}'.format(path))
                with path.open('rb') as stream:
                    if stream.read(4) == b'\x7fELF' and relative not in ('usr/lib/zarnitsa/zarnitsa', 'usr/lib/zarnitsa/zarnitsa-icmp'):
                        fail('В payload обнаружен посторонний ELF {}'.format(relative))
                actual_files.add(relative)
            else:
                fail('Специальный файл в payload {}'.format(path))
            os.utime(str(path), (epoch, epoch), follow_symlinks=False)
        if actual_files != allowed_files:
            fail('Payload whitelist не совпал: {}'.format(sorted(actual_files ^ allowed_files)))

        # Публикуются только проверенные полностью собранные артефакты. --root-owner-group
        # задаёт uid/gid 0 внутри .deb без chown исходников и без fakeroot/host install.
        dist.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='.system-qt-', dir=str(dist)) as artifact_work:
            artifact_work = Path(artifact_work)
            deb = artifact_work / ASSET
            run(['dpkg-deb', '--build', '--root-owner-group', stage, deb])
            for field, expected in (('Package', 'zarnitsa'), ('Version', deb_version), ('Architecture', ARCH),
                                    ('Depends', depends), ('Installed-Size', str(installed_size))):
                if output('dpkg-deb', '--field', deb, field) != expected:
                    fail('Некорректное поле {} готового .deb'.format(field))
            if 'stefanvoskres@gmail.com' not in output('dpkg-deb', '--field', deb, 'Maintainer'):
                fail('Maintainer не совпадает с контактом LICENSE')

            def verify_tar(option, expected_files, control_archive=False):
                result = subprocess.run(['dpkg-deb', option, str(deb)], env=ENV,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
                seen = set()
                seen_members = set()
                with tarfile.open(fileobj=io.BytesIO(result.stdout), mode='r:') as archive:
                    for member in archive:
                        name = member.name
                        if name.startswith('./'):
                            name = name[2:]
                        name = name.rstrip('/') or '.'
                        relative = PurePosixPath(name)
                        if relative.is_absolute() or '..' in relative.parts or name in seen_members:
                            fail('Небезопасное/повторное имя в deb tar: {}'.format(member.name))
                        seen_members.add(name)
                        if member.uid or member.gid or member.mode & 0o6000 or member.pax_headers:
                            fail('Не root-owned либо неожиданные privileges/xattrs tar: {}'.format(name))
                        if member.isdir():
                            if member.mode != 0o755 or (name != '.' and (control_archive or name not in allowed_dirs)):
                                fail('Посторонний каталог/режим в deb tar: {}'.format(name))
                            continue
                        if not member.isfile() or name not in expected_files:
                            fail('Посторонний файл/ссылка в deb tar: {}'.format(name))
                        original = stage / ('DEBIAN/' + name if control_archive else name)
                        if member.mode != stat.S_IMODE(original.stat().st_mode):
                            fail('Режим tar отличается от stage: {}'.format(name))
                        stream = archive.extractfile(member)
                        if stream is None or hashlib.sha256(stream.read()).hexdigest() != sha(original):
                            fail('Содержимое tar отличается от stage: {}'.format(name))
                        seen.add(name)
                if seen != expected_files:
                    fail('Файлы готового .deb не совпали с whitelist: {}'.format(sorted(seen ^ expected_files)))

            verify_tar('--fsys-tarfile', {name for name in allowed_files if not name.startswith('DEBIAN/')})
            verify_tar('--ctrl-tarfile', {'control', 'postinst'}, control_archive=True)
            if source_fingerprint() != source_sha:
                fail('Build inputs изменились до публикации; .deb не выпускается')
            check_git()
            for name, previous_sha in original_sha.items():
                if sha(build / name) != previous_sha:
                    fail('Build binary изменился до публикации: {}'.format(name))
            write_text(artifact_work / 'SHA256SUMS', '{}  {}\n'.format(sha(deb), ASSET))
            published = []
            try:
                for name in (ASSET, 'SHA256SUMS'):
                    temporary = artifact_work / name
                    temporary.chmod(0o644)
                    # link в том же output fs атомарен и в отличие от replace/rename
                    # никогда не перезаписывает уже существовавший непроверенный файл.
                    os.link(str(temporary), str(dist / name))
                    published.append(dist / name)
                if stage_target.exists() or stage_target.is_symlink():
                    fail('Stage появился во время сборки; не заменяю {}'.format(stage_target))
                os.rename(str(stage), str(stage_target))
            except BaseException:
                for path in published:
                    path.unlink()
                raise
        print('\nГотов native stage: {}'.format(stage_target), flush=True)
        print('ctest: {} наборов; QtTest: {} passed, {} failed, {} skipped, {} blacklisted'.format(
            len(test_names), *test_counts), flush=True)
        print('SourceCommit={}; GitDirty={}'.format(commit, dirty), flush=True)
        print('{}  {}  ({:.2f} MiB)'.format(sha(dist / ASSET), dist / ASSET,
                                            (dist / ASSET).stat().st_size / 1024.0 ** 2), flush=True)
        print('SHA256SUMS: {}'.format(dist / 'SHA256SUMS'), flush=True)
    # Ссылки сохраняются в этой области до завершения всех операций.
    assert len(locks) == 2


try:
    main()
except subprocess.CalledProcessError as error:
    if error.stdout:
        print(error.stdout.decode('utf-8', 'replace') if isinstance(error.stdout, bytes) else error.stdout, file=sys.stderr)
    if error.stderr:
        print(error.stderr.decode('utf-8', 'replace') if isinstance(error.stderr, bytes) else error.stderr, file=sys.stderr)
    print('Сборка остановлена: команда завершилась с кодом {}'.format(error.returncode), file=sys.stderr)
    sys.exit(error.returncode if 0 < error.returncode < 256 else 1)
except (RuntimeError, OSError, ValueError) as error:
    print('Сборка остановлена: {}'.format(error), file=sys.stderr)
    sys.exit(1)
PY
