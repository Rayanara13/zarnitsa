#!/bin/bash
# Сборщик офлайн-комплекта. Запускать в zarnitsa-build:astra17, а не на целевой машине.
# Обязательные ZARNITSA_* с ревизией и digest передаёт вызывающий docker run на хосте.
# --runtime-only проверяет сборку и собирает stage, но НЕ выпускает неполные tar/deb.
set -euo pipefail
export LANG=C.UTF-8 LC_ALL=C.UTF-8
umask 022

for arg in "$@"; do
    case "$arg" in
        --help|-h)
            printf '%s\n' \
                'Использование: bash docker/package.sh [--runtime-only [--reuse-build]]' \
                'Среда: ZARNITSA_SOURCE_DIR=/src, ZARNITSA_BUILD_DIR=/build/full,' \
                'ZARNITSA_PACKAGE_DIR=/build/package, ZARNITSA_DIST_DIR=/src/dist.' \
                'Обязательные metadata: ZARNITSA_SOURCE_COMMIT, ZARNITSA_REVISION,' \
                'ZARNITSA_GIT_DIRTY=clean|dirty, ZARNITSA_DOCKER_IMAGE_DIGEST,' \
                'ZARNITSA_BASE_IMAGE_DIGEST, ZARNITSA_BASE_PACKAGE_VERSIONS.' \
                'Дополнительно: ZARNITSA_DOCKER_IMAGE_ID, ZARNITSA_BASE_IMAGE_DIGEST,' \
                'ZARNITSA_BUILD_DATE (UTC, YYYY-MM-DDTHH:MM:SSZ), SOURCE_DATE_EPOCH.' \
                'Обычно выполняются Release GUI/tests ON и ctest.' \
                '--reuse-build: только предварительный runtime-only без сборки/тестов/выпуска.'
            exit 0
            ;;
        --runtime-only|--reuse-build) ;;
        *) printf 'Неизвестный аргумент: %s\n' "$arg" >&2; exit 2 ;;
    esac
done
if [[ "$(uname -s)" != Linux ]]; then
    printf '%s\n' 'Этот скрипт запускается внутри Linux-образа zarnitsa-build:astra17.' >&2
    exit 2
fi
export ZARNITSA_SOURCE_DIR="${ZARNITSA_SOURCE_DIR:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)}"

# Изолированный Python нужен только сборщику. В готовом комплекте Python не требуется.
python3 -I - "$@" <<'PY'
import csv
import datetime
import fcntl
import gzip
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

RUNTIME_ONLY = "--runtime-only" in sys.argv[1:]
REUSE_BUILD = "--reuse-build" in sys.argv[1:]
if REUSE_BUILD and not RUNTIME_ONLY:
    raise RuntimeError("--reuse-build разрешён только вместе с --runtime-only; full всегда пересобирается и тестируется")
VERSION = "1.0.0"
SOURCE = Path(os.environ["ZARNITSA_SOURCE_DIR"]).resolve(strict=True)
BUILD = Path(os.environ.get("ZARNITSA_BUILD_DIR", "/build/full")).resolve()
PACKAGE = Path(os.environ.get("ZARNITSA_PACKAGE_DIR", str(BUILD.parent / "package"))).resolve()
DIST = Path(os.environ.get("ZARNITSA_DIST_DIR", str(SOURCE / "dist"))).resolve()
STAGE = PACKAGE / "Zarnitsa"
ENV = os.environ.copy()
for key in ("LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML2_IMPORT_PATH"):
    ENV.pop(key, None)
ENV.update(LANG="C.UTF-8", LC_ALL="C")


def fail(message):
    raise RuntimeError(message)


def run(args, cwd=None, env=None, capture=False):
    # Сборка и ctest наследуют stdout/stderr; их код возврата не теряется в pipeline.
    result = subprocess.run([str(a) for a in args], cwd=str(cwd) if cwd else None,
                            env=env or ENV, check=True,
                            stdout=subprocess.PIPE if capture else None,
                            stderr=subprocess.PIPE if capture else None,
                            universal_newlines=True)
    return result.stdout.strip() if capture else None


def output(*args):
    return run(args, capture=True)


def sha(path):
    digest = hashlib.sha256()
    with open(str(path), "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def text(path, content, mode=0o644):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(content)
    path.chmod(mode)


def plain_copy(source, target, mode=0o644):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(str(source), str(target))
    # copyfile не переносит setuid, capabilities, ACL и чужие xattrs.
    target.chmod(mode)


def inside(path, root):
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def safe_name(name):
    if not name or any(c in name for c in "\n\r\t\\"):
        fail("Небезопасное имя файла: {!r}".format(name))


def metadata(name, pattern):
    value = os.environ.get(name, "")
    if not re.fullmatch(pattern, value):
        fail("Нужно передать достоверное {} (получено {!r})".format(name, value))
    return value


COMMIT = metadata("ZARNITSA_SOURCE_COMMIT", r"(?:[0-9a-f]{40}|[0-9a-f]{64})")
REVISION = metadata("ZARNITSA_REVISION", r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
DIRTY = metadata("ZARNITSA_GIT_DIRTY", r"(?:clean|dirty)")
IMAGE_DIGEST = metadata("ZARNITSA_DOCKER_IMAGE_DIGEST", r"(?:[^\s]+@)?sha256:[0-9a-f]{64}")
IMAGE_ID = os.environ.get("ZARNITSA_DOCKER_IMAGE_ID", "не передан")
BASE_DIGEST = os.environ.get("ZARNITSA_BASE_IMAGE_DIGEST", "не передан")
if IMAGE_ID != "не передан" and not re.fullmatch(r"sha256:[0-9a-f]{64}", IMAGE_ID):
    fail("Некорректный ZARNITSA_DOCKER_IMAGE_ID")
if not re.fullmatch(r"[^\s]+@sha256:[0-9a-f]{64}", BASE_DIGEST):
    fail("Нужен точный ZARNITSA_BASE_IMAGE_DIGEST для проверки системных Astra dependencies")
EXTERNAL_ASTRA_BASE = {"libgost", "libparsec-base3", "libpdp"}
# Минимумы 1.0.0 подтверждены forced-bundled запуском на живой Astra SE 1.7.6.
# Это НЕ версии build image. Последние отдельно приходят в BASE_PACKAGE_VERSIONS.
TESTED_BASE_MINIMUM_VERSIONS = {
    "libgost": "2.0.2-5+ci4",
    "libparsec-base3": "3.4+ci71",
    "libpdp": "3.4+ci71",
}
TESTED_BASE_MINIMUM_PROFILE = "Astra Linux SE 1.7.6; glibc 2.28; forced bundled Qt/OpenSSL; без sudo"
TESTED_BASE_MINIMUM_EVIDENCE = "26.11.2026; candidate d213e4426d31-20261126T103359Z-dirty; workspace/lab-bundled-candidate1/result.json + remote.log; ldd-r, offscreen GUI, HTTPS API, pinned HTTPS probe Up"
TESTED_BASE_PRIVILEGED_VALIDATION = "NOT_PERFORMED: sudo, МКЦ и реальные file capabilities на живой Astra SE 1.7.6 не проверялись"
BASE_PACKAGE_VERSIONS = {}
for item in re.split(r"[;\r\n]+", os.environ.get("ZARNITSA_BASE_PACKAGE_VERSIONS", "")):
    if not item:
        continue
    match = re.fullmatch(r"(libgost|libparsec-base3|libpdp)=([0-9A-Za-z.+:~_-]+)", item)
    if not match or match.group(1) in BASE_PACKAGE_VERSIONS:
        fail("Некорректный ZARNITSA_BASE_PACKAGE_VERSIONS; нужны уникальные package=version")
    BASE_PACKAGE_VERSIONS[match.group(1)] = match.group(2)
if set(BASE_PACKAGE_VERSIONS) != EXTERNAL_ASTRA_BASE:
    fail("Передайте фактический dpkg-query из bare base image: ZARNITSA_BASE_PACKAGE_VERSIONS (libgost, libparsec-base3, libpdp)")
for package, version in sorted(BASE_PACKAGE_VERSIONS.items()):
    if output("dpkg-query", "-W", "-f=${Version}", package) != version:
        fail("Версии {} в bare base metadata и build image различаются".format(package))
BUILD_DATE = os.environ.get("ZARNITSA_BUILD_DATE", datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%SZ"))
if not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z", BUILD_DATE):
    fail("ZARNITSA_BUILD_DATE должен быть UTC ISO8601 с Z")
build_time = datetime.datetime.strptime(BUILD_DATE, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=datetime.timezone.utc)
EPOCH = int(os.environ.get("SOURCE_DATE_EPOCH", str(int(build_time.timestamp()))))
if EPOCH < 0:
    fail("SOURCE_DATE_EPOCH не может быть отрицательным")
if PACKAGE in (Path("/"), SOURCE, BUILD) or inside(SOURCE, PACKAGE):
    fail("Небезопасный каталог staging: {}".format(PACKAGE))
if output("dpkg", "--print-architecture") != "amd64":
    fail("Этот комплект рассчитан только на Astra x86_64/amd64")
if output("getconf", "GNU_LIBC_VERSION") != "glibc 2.28":
    fail("Требуется базовый ABI glibc 2.28 из Astra ubi17")

# Если git установлен, не позволяем переданному clean скрыть реальные изменения.
if shutil.which("git", path=ENV.get("PATH")):
    actual_commit = output("git", "-C", SOURCE, "rev-parse", "HEAD")
    actual_dirty = bool(output("git", "-C", SOURCE, "status", "--porcelain", "--untracked-files=all"))
    if actual_commit != COMMIT or (actual_dirty and DIRTY != "dirty"):
        fail("Metadata хоста расходится с текущим git-деревом")

SOURCE_ROOTS = ("CMakeLists.txt", "src", "include", "resources", "tests", "docker", "packaging", "cmake", "translations", "docs",
                "README.md", "ИНСТРУКЦИЯ.txt", "LICENSE", "LICENSE.txt", ".gitattributes", ".gitignore")
SKIP_DIRS = {".git", "dist", "__pycache__", ".cache", ".venv", "venv", "node_modules", ".idea", ".vscode"}
PRIVATE_SUFFIXES = {".key", ".pem", ".p12", ".pfx", ".sqlite", ".sqlite3", ".db"}
# Публичные локальные self-signed fixtures тестов; это НЕ рабочие сертификаты/ключи.
PUBLIC_TEST_FIXTURES = {"tests/data/test-cert.pem", "tests/data/test-key.pem"}


def source_paths():
    # Белый список корней программы: не архивируем home, .git, dist, кэши и пользовательские конфиги.
    result = []
    for name in SOURCE_ROOTS:
        root = SOURCE / name
        if not root.exists() and not root.is_symlink():
            continue
        candidates = [root]
        if root.is_dir() and not root.is_symlink():
            for current, dirs, files in os.walk(str(root), followlinks=False):
                dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS and not re.match(r"build(?:$|[-_])", d))
                candidates.extend(Path(current) / d for d in dirs)
                candidates.extend(Path(current) / f for f in sorted(files))
        for path in candidates:
            safe_name(str(path.relative_to(SOURCE)))
            public_fixture = path.relative_to(SOURCE).as_posix() in PUBLIC_TEST_FIXTURES and path.is_file() and not path.is_symlink()
            if (path.name.startswith(".env") or path.suffix.lower() in PRIVATE_SUFFIXES) and not public_fixture:
                fail("В дереве исходников обнаружен потенциально приватный файл; проверьте его отдельно: {}".format(path))
            if path.suffix in (".pyc", ".pyo", ".log", ".user", ".swp") or path.name.endswith("~"):
                continue
            if path.is_symlink() and not inside(path.resolve(strict=True), SOURCE):
                fail("Исходная ссылка выходит за дерево проекта: {}".format(path))
            result.append(path)
    return sorted(set(result), key=lambda p: p.relative_to(SOURCE).as_posix())


def source_fingerprint(paths, code_only=False):
    result = hashlib.sha256()
    for path in paths:
        rel = path.relative_to(SOURCE)
        if code_only and rel.parts[0] not in ("CMakeLists.txt", "src", "include", "resources", "tests", "cmake"):
            continue
        result.update(rel.as_posix().encode("utf-8") + b"\0")
        if path.is_symlink():
            result.update(b"link\0" + os.readlink(str(path)).encode("utf-8"))
        elif path.is_file():
            result.update(sha(path).encode("ascii"))
        else:
            result.update(b"dir")
        result.update(b"\0")
    return result.hexdigest()


PACKAGE.mkdir(parents=True, exist_ok=True)
lock = open(str(PACKAGE / ".package.lock"), "a")
try:
    fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
except BlockingIOError:
    fail("Уже работает другой сборщик комплекта")

code_before = None
if not REUSE_BUILD:
    code_before = source_fingerprint(source_paths(), code_only=True)
    run(["cmake", "-S", SOURCE, "-B", BUILD, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         "-DZARNITSA_GUI=ON", "-DZARNITSA_TESTS=ON"])
    run(["cmake", "--build", BUILD, "--parallel", str(os.cpu_count() or 1)])
    test_env = ENV.copy()
    test_env["QT_QPA_PLATFORM"] = "offscreen"
    run(["ctest", "--output-on-failure"], cwd=BUILD, env=test_env)
    if source_fingerprint(source_paths(), code_only=True) != code_before:
        fail("Исходники изменились во время сборки/ctest; повторите упаковку после завершения параллельных правок")
else:
    print("Предварительный runtime: существующие binaries, cmake/ctest НЕ запускались; это НЕ релиз.", flush=True)

cache = {}
for line in (BUILD / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
    match = re.match(r"([^#/:][^:]*):[^=]+=(.*)", line)
    if match:
        cache[match.group(1)] = match.group(2)
if cache.get("CMAKE_PROJECT_VERSION") != VERSION:
    fail("Версия CMake не совпадает с версией комплекта {}".format(VERSION))
QT_VERSION = output("qmake", "-query", "QT_VERSION")
if not QT_VERSION.startswith("5.15."):
    fail("Для Astra-комплекта требуется Qt 5.15.x")
QT_PLUGINS = Path(output("qmake", "-query", "QT_INSTALL_PLUGINS")).resolve(strict=True)
COMPILER = cache.get("CMAKE_CXX_COMPILER", "c++")

stage = Path(tempfile.mkdtemp(prefix=".Zarnitsa-", dir=str(PACKAGE)))
stage.chmod(0o755)
manifest = {}
system_manifest = {}
packages = {}
owner_cache = {}
visited = set()
copied_libraries = {}
missing_licenses = []
original_binary_sha = {}


def owner(path):
    key = str(path)
    if key in owner_cache:
        return owner_cache[key]
    real = path.resolve(strict=True)
    candidates = [str(path), str(real)]
    for name in list(candidates):
        if name.startswith(("/lib/", "/lib64/", "/bin/", "/sbin/")):
            candidates.append("/usr" + name)
        elif name.startswith(("/usr/lib/", "/usr/lib64/", "/usr/bin/", "/usr/sbin/")):
            candidates.append(name[4:])
    for candidate in dict.fromkeys(candidates):
        result = subprocess.run(["dpkg-query", "-S", candidate], env=ENV, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, universal_newlines=True)
        if result.returncode:
            continue
        for line in result.stdout.splitlines():
            if ": " not in line or line.startswith(("diversion ", "local diversion ")):
                continue
            names, found = line.split(": ", 1)
            if found != candidate:
                continue
            package = names.split(", ")[0]
            value = output("dpkg-query", "-W", "-f=${binary:Package}\t${Version}\t${source:Package}\t${source:Version}", package).split("\t")
            if len(value) != 4 or not all(value):
                fail("Не удалось определить Source/Version для {}".format(package))
            info = dict(zip(("binary_package", "binary_version", "source_package", "source_version"), value))
            packages[info["binary_package"]] = info
            owner_cache[key] = info
            return info
    fail("Нет исходного dpkg-пакета для {} ({})".format(path, real))


def record(target, source, kind, link_target=""):
    rel = target.relative_to(stage).as_posix()
    if rel in manifest:
        if manifest[rel]["link_target"] != link_target:
            fail("Изменена цель уже записанной ссылки: {}".format(rel))
        return
    info = owner(source)
    row = dict(info)
    row.update(path=rel, kind=kind, source_path=str(source), resolved_source_path=str(source.resolve(strict=True)),
               sha256=sha(target), link_target=link_target)
    manifest[rel] = row


def copy_elf(source, target, kind, strip=False):
    if not source.is_file():
        fail("Отсутствует ELF: {}".format(source))
    header = output("readelf", "-h", source)
    if "ELF64" not in header or "X86-64" not in header:
        fail("Ожидался ELF64 x86-64: {}".format(source))
    if target.exists() or target.is_symlink():
        if target.is_symlink() or sha(source) != sha(target):
            fail("Конфликт имени библиотеки: {}".format(target))
        return
    plain_copy(source, target, 0o755)
    if strip:
        run(["strip", "--strip-unneeded", target])
    if kind != "application":
        record(target, source, kind)


def link_library(name, actual, source):
    safe_name(name)
    if not re.fullmatch(r"[A-Za-z0-9_.+-]+", name):
        fail("Некорректный SONAME: {}".format(name))
    target = stage / "runtime/lib" / name
    if target.name == actual.name:
        return
    if target.is_symlink():
        if os.readlink(str(target)) != actual.name:
            fail("Разные библиотеки претендуют на {}".format(name))
    elif target.exists():
        fail("Имя SONAME занято обычным файлом: {}".format(name))
    else:
        target.symlink_to(actual.name)
    record(target, source, "library-symlink", actual.name)


def library(soname, source):
    real = source.resolve(strict=True)
    info = owner(source)
    # Не переносим НИ ОДНУ библиотеку glibc, в том числе libcrypt/libutil/libanl.
    excluded = info["source_package"] == "glibc" or re.match(
        r"(?:ld-linux|ld-[0-9]|lib(?:c|pthread|m|dl|rt|resolv|nss_[^.]+|util|anl|crypt)\.so)", soname)
    base_package = info["binary_package"].split(":")[0]
    external_astra = base_package in EXTERNAL_ASTRA_BASE
    if excluded or external_astra:
        key = (soname, str(source))
        evidence = "системная glibc Astra; getconf GNU_LIBC_VERSION=glibc 2.28"
        if external_astra:
            if info["binary_version"] != BASE_PACKAGE_VERSIONS[base_package]:
                fail("Неподтверждённая версия external base {}".format(base_package))
            evidence = "bare image {}; host dpkg-query {}={}; совпадает с builder".format(BASE_DIGEST, base_package, BASE_PACKAGE_VERSIONS[base_package])
        system_manifest[key] = dict(info, soname=soname, source_path=str(source), resolved_source_path=str(real),
                                    reason="external-Astra-base" if external_astra else "system-glibc-ABI", verification=evidence,
                                    minimum_tested_version=TESTED_BASE_MINIMUM_VERSIONS[base_package] if external_astra else "",
                                    minimum_tested_profile=TESTED_BASE_MINIMUM_PROFILE if external_astra else "",
                                    minimum_tested_evidence=TESTED_BASE_MINIMUM_EVIDENCE if external_astra else "",
                                    minimum_profile_privileged_validation=TESTED_BASE_PRIVILEGED_VALIDATION if external_astra else "")
        return
    if not any(inside(real, Path(p).resolve()) for p in ("/lib", "/lib64", "/usr/lib", "/usr/lib64")):
        fail("Зависимость находится вне системных библиотек сборочного образа: {}".format(source))
    target = stage / "runtime/lib" / real.name
    if str(real) not in copied_libraries:
        copy_elf(real, target, "shared-library")
        copied_libraries[str(real)] = target
        match = re.search(r"\(SONAME\).*\[([^\]]+)\]", output("readelf", "-d", real))
        if match:
            link_library(match.group(1), target, source)
    link_library(source.name, target, source)
    link_library(soname, target, source)
    dependencies(real)


def dependencies(path):
    real = path.resolve(strict=True)
    if str(real) in visited:
        return
    visited.add(str(real))
    dynamic = output("readelf", "-d", real)
    if "There is no dynamic section" in dynamic or "(NEEDED)" not in dynamic:
        # Например, libicudata — полноценный data-only ELF .so без DT_NEEDED;
        # ldd законно отвечает для него «statically linked».
        return
    result = subprocess.run(["ldd", str(real)], env=ENV, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, universal_newlines=True)
    if result.returncode or "not found" in result.stdout:
        fail("Не разрешены зависимости {}:\n{}{}".format(real, result.stdout, result.stderr))
    for line in result.stdout.splitlines():
        match = re.match(r"\s*(\S+)\s+=>\s+(/\S+)\s+\(", line)
        if match:
            library(match.group(1), Path(match.group(2)))
            continue
        match = re.match(r"\s*(/\S+)\s+\(", line)
        if match:
            library(Path(match.group(1)).name, Path(match.group(1)))
        elif line.strip() and not line.lstrip().startswith("linux-vdso.so."):
            fail("Неожиданный вывод ldd для {}: {}".format(real, line))


def tsv(path, rows, columns):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns, delimiter="\t", lineterminator="\n", extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)
    path.chmod(0o644)


def license_file(info):
    package = info["binary_package"].split(":")[0]
    for filename in ("copyright", "copyright.gz"):
        path = Path("/usr/share/doc") / package / filename
        if path.is_file():
            if not inside(path.resolve(strict=True), Path("/usr/share/doc")):
                fail("copyright вне /usr/share/doc: {}".format(path))
            target = stage / "licenses/runtime" / package / "copyright"
            if filename.endswith(".gz"):
                with gzip.open(str(path), "rb") as stream:
                    data = stream.read()
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
                target.chmod(0o644)
            else:
                plain_copy(path, target)
            return str(path)
    if RUNTIME_ONLY:
        missing_licenses.append(package)
        text(stage / "licenses/runtime" / package / "MISSING-COPYRIGHT.txt",
             "ВНИМАНИЕ: исходный Astra binary package не предоставил copyright.\n"
             "Это промежуточный runtime-only stage, не готовый к распространению релиз.\n"
             "Пакет: {}\nВерсия: {}\nSource: {}={}\n".format(package, info["binary_version"], info["source_package"], info["source_version"]))
        return "MISSING (runtime-only; полный выпуск запрещён)"
    fail("Отсутствует обязательный copyright для {}".format(package))


def all_paths(root):
    result = []
    for current, dirs, files in os.walk(str(root), followlinks=False):
        for name in sorted(dirs + files):
            result.append(Path(current) / name)
    return sorted(result, key=lambda p: p.relative_to(root).as_posix())


def validate_links(root):
    for path in all_paths(root):
        safe_name(path.relative_to(root).as_posix())
        if path.is_symlink():
            target = os.readlink(str(path))
            if os.path.isabs(target) or not inside(path.resolve(strict=True), root):
                fail("Небезопасная ссылка: {} -> {}".format(path, target))
        elif not (path.is_file() or path.is_dir()):
            fail("Специальный файл в комплекте: {}".format(path))
        if path.lstat().st_mode & 0o6000:
            fail("setuid/setgid недопустим в переносном комплекте: {}".format(path))


def checksums(root):
    validate_links(root)
    rows = []
    for path in all_paths(root):
        if path.is_file() and not path.is_symlink() and path != root / "SHA256SUMS":
            rows.append("{}  {}\n".format(sha(path), path.relative_to(root).as_posix()))
    text(root / "SHA256SUMS", "".join(rows))


def archive(target, roots, base):
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=EPOCH) as compressed:
            with tarfile.open(fileobj=compressed, mode="w|", format=tarfile.PAX_FORMAT) as tar:
                for path, arcname in roots:
                    info = tar.gettarinfo(str(path), arcname=arcname)
                    info.uid = info.gid = 0
                    info.uname = info.gname = "root"
                    info.mtime = EPOCH
                    info.pax_headers = {}
                    # Windows bind mount может показывать исходникам 0777: не несём
                    # group/world-writable или setuid-биты в архив пользователя.
                    if info.isdir():
                        info.mode = 0o755
                    elif info.isfile():
                        info.mode = 0o755 if info.mode & 0o111 else 0o644
                    if info.issym():
                        resolved = path.resolve(strict=True)
                        if os.path.isabs(info.linkname) or not inside(resolved, base):
                            fail("Небезопасная ссылка в архиве: {}".format(path))
                    if info.isfile():
                        with path.open("rb") as stream:
                            tar.addfile(info, stream)
                    else:
                        tar.addfile(info)
    target.chmod(0o644)


def make_deb(work, target):
    debroot = work / "deb-root"
    kit = debroot / "opt/zarnitsa-deb"
    kit.parent.mkdir(parents=True)
    shutil.copytree(str(stage), str(kit), symlinks=True)
    text(kit / "DEBIAN_PACKAGE", "ZARNITSA_DEBIAN_PACKAGE=1\nPACKAGE=zarnitsa\nLAYOUT=/opt/zarnitsa-deb\nУдаление и обновление — через dpkg, не через install.sh/uninstall.sh.\n")
    checksums(kit)
    text(debroot / "usr/bin/zarnitsa", '#!/bin/bash\nset -euo pipefail\nexec /opt/zarnitsa-deb/zarnitsa.sh "$@"\n', 0o755)
    desktop = (kit / "zarnitsa.desktop.in").read_text(encoding="utf-8")
    replacements = {"@ZARNITSA_EXEC@": "/usr/bin/zarnitsa", "@ZARNITSA_TRY_EXEC@": "/usr/bin/zarnitsa",
                    "@ZARNITSA_ROOT@": "/opt/zarnitsa-deb"}
    for old, new in replacements.items():
        desktop = desktop.replace(old, new)
    if re.search(r"@[A-Z_]+@", desktop) or re.search(r"%[FUfu]", desktop):
        fail("Неизвестные placeholders или файловые аргументы в zarnitsa.desktop.in")
    if not re.search(r"(?m)^Exec=/usr/bin/zarnitsa(?:\s|$)", desktop):
        fail("Шаблон .desktop не дал ожидаемый /usr/bin/zarnitsa")
    text(debroot / "usr/share/applications/zarnitsa.desktop", desktop)
    for icon in sorted((stage / "icons").glob("*x*/apps/zarnitsa.png")):
        plain_copy(icon, debroot / "usr/share/icons/hicolor" / icon.relative_to(stage / "icons"))
    copyright_text = "Зарница {}\n\n".format(VERSION)
    if (stage / "LICENSE").is_file():
        copyright_text += (stage / "LICENSE").read_text(encoding="utf-8") + "\n\n"
    copyright_text += (stage / "THIRD-PARTY-NOTICES.txt").read_text(encoding="utf-8")
    text(debroot / "usr/share/doc/zarnitsa/copyright", copyright_text)
    control = (SOURCE / "packaging/debian/control.in").read_text(encoding="utf-8")
    deb_version = VERSION + "-1+g" + re.sub(r"[^a-z0-9.+]", ".", REVISION.lower())
    installed_size = sum((p.stat().st_size + 1023) // 1024 for p in all_paths(debroot) if p.is_file() and not p.is_symlink())
    # Более новые фактические build/bare версии не должны отсеивать проверенную 1.7.6.
    base_depends = ", ".join("{} (>= {})".format(name, version) for name, version in sorted(TESTED_BASE_MINIMUM_VERSIONS.items()))
    control = control.replace("@VERSION@", deb_version).replace("@INSTALLED_SIZE@", str(installed_size)).replace("@ASTRA_BASE_MINIMUM_DEPENDS@", base_depends)
    if re.search(r"@[A-Z_]+@", control):
        fail("Неизвестный placeholder в Debian control")
    text(debroot / "DEBIAN/control", control)
    plain_copy(SOURCE / "packaging/debian/postinst", debroot / "DEBIAN/postinst", 0o755)
    run(["bash", "-n", debroot / "DEBIAN/postinst"])
    # Никаких install.sh, apt или maintainer scripts во время самой сборки .deb.
    run(["dpkg-deb", "--build", "--root-owner-group", debroot, target])
    run(["dpkg-deb", "--info", target])


try:
    for name in ("zarnitsa", "zarnitsa-icmp"):
        source = BUILD / name
        original_binary_sha[name] = sha(source)
        copy_elf(source, stage / "bin" / name, "application", strip=True)
        dependencies(source)
    if "INTERP" in output("readelf", "-l", BUILD / "zarnitsa-icmp"):
        fail("ICMP helper должен быть статическим")
    # ldd не видит эти dlopen-зависимости Qt Network. Они обязательны даже без HTTPS-теста.
    ld_cache = output("/sbin/ldconfig", "-p")
    for soname in ("libssl.so.1.1", "libcrypto.so.1.1", "libstdc++.so.6", "libgcc_s.so.1"):
        found = re.search(r"(?m)^\s*" + re.escape(soname) + r"\s+\([^\n]*x86-64[^\n]*\)\s+=>\s+(\S+)$", ld_cache)
        if not found:
            fail("В сборочном образе отсутствует {}".format(soname))
        library(soname, Path(found.group(1)))
    for name in ("ping", "openssl", "setcap", "getcap"):
        location = shutil.which(name, path=ENV.get("PATH"))
        if not location:
            fail("Для полного offline runtime нужен {} в сборочном образе".format(name))
        source = Path(location)
        copy_elf(source, stage / "runtime/bin" / name, "runtime-tool", strip=True)
        dependencies(source)

    selected = []
    for name in ("libqxcb.so", "libqoffscreen.so", "libqminimal.so"):
        path = QT_PLUGINS / "platforms" / name
        if not path.is_file():
            fail("Отсутствует обязательный Qt platform plugin {}".format(path))
        selected.append(path)
    for directory in ("imageformats", "bearer", "xcbglintegrations", "platforminputcontexts", "platformthemes", "styles", "iconengines", "accessiblebridge"):
        root = QT_PLUGINS / directory
        if root.is_dir():
            selected.extend(sorted(p for p in root.rglob("*.so") if p.is_file()))
    for source in sorted(set(selected)):
        copy_elf(source, stage / "runtime/plugins" / source.relative_to(QT_PLUGINS), "qt-plugin")
        dependencies(source)

    # xkbcommon нужны не только .so, но и таблицы клавиатуры на минимальном ubi17.
    xkb_root = Path("/usr/share/X11/xkb")
    if not xkb_root.is_dir():
        fail("В сборочном образе отсутствует xkb-data")
    for source in all_paths(xkb_root):
        target = stage / "runtime/share/X11/xkb" / source.relative_to(xkb_root)
        if source.is_symlink():
            if not source.resolve(strict=True).is_file() or not inside(source.resolve(strict=True), xkb_root):
                fail("Неожиданная ссылка в xkb-data: {}".format(source))
        if source.is_file():
            plain_copy(source, target)
            record(target, source, "xkb-data")
        elif source.is_dir():
            target.mkdir(parents=True, exist_ok=True)

    # Собственный минимальный fontconfig, а не копия /etc/fonts сборочной машины.
    # Нет DTD/include и приватных путей; встроенные в Qt ресурсы шрифтов уже в executable.
    text(stage / "runtime/etc/fonts/fonts.conf", '''<?xml version="1.0" encoding="UTF-8"?>
<fontconfig>
  <dir>/usr/share/fonts</dir>
  <dir>/usr/local/share/fonts</dir>
  <dir prefix="xdg">fonts</dir>
  <cachedir prefix="xdg">fontconfig</cachedir>
  <config><rescan><int>0</int></rescan></config>
</fontconfig>
''')

    render_env = ENV.copy()
    render_env["QT_QPA_PLATFORM"] = "offscreen"
    run([BUILD / "zarnitsa", "--render-icons", stage / "icons"], env=render_env)
    if not (stage / "icons/48x48/apps/zarnitsa.png").is_file():
        fail("Рендер значков не создал icons/48x48/apps/zarnitsa.png")
    for source in sorted((SOURCE / "resources/licenses").glob("*")):
        if source.is_file():
            plain_copy(source, stage / "licenses/fonts" / source.name)
    if not list((stage / "licenses/fonts").glob("*")):
        fail("Отсутствуют лицензии встроенных шрифтов")

    required_scripts = ("zarnitsa.sh", "zarnitsa-common.sh", "install.sh", "uninstall.sh", "configure-icmp.sh",
                        "check-runtime.sh", "diagnose.sh", "make-cert.sh")
    for source in sorted((SOURCE / "packaging").glob("*.sh")):
        if b"\r" in source.read_bytes():
            fail("Shell-файл должен иметь LF, а не CRLF: {}".format(source))
        run(["bash", "-n", source])
        plain_copy(source, stage / source.name, 0o755)
    desktop = SOURCE / "packaging/zarnitsa.desktop.in"
    if desktop.is_file():
        plain_copy(desktop, stage / desktop.name)
    if not RUNTIME_ONLY:
        missing = [name for name in required_scripts + ("zarnitsa.desktop.in",) if not (stage / name).is_file()]
        missing += [name for name in ("README.md", "ИНСТРУКЦИЯ.txt") if not (SOURCE / name).is_file()]
        if missing:
            fail("Полный комплект ещё не готов; отсутствуют: " + ", ".join(missing) + ". Используйте --runtime-only")
    for name in ("LICENSE", "README.md", "ИНСТРУКЦИЯ.txt"):
        if (SOURCE / name).is_file():
            plain_copy(SOURCE / name, stage / name)
    for name in ("docs/zarnitsa.png", "docs/download-astra.svg"):
        preview = Path(name)
        if (SOURCE / preview).is_file():
            plain_copy(SOURCE / preview, stage / preview)
    plain_copy(SOURCE / "packaging/third-party-notices.txt", stage / "THIRD-PARTY-NOTICES.txt")
    text(stage / "VERSION.txt", "VERSION={}\nREVISION={}\nBUILD_DATE={}\n".format(VERSION, REVISION, BUILD_DATE))

    # Статический helper содержит glibc, поэтому сохраняем её notices, хотя саму glibc не копируем.
    owner(Path("/lib/x86_64-linux-gnu/libc.so.6"))
    package_rows = []
    for name, info in sorted(packages.items()):
        if name.split(":")[0] in EXTERNAL_ASTRA_BASE:
            # Эти MAC/base компоненты НЕ перераспространяем и не выдумываем их лицензии.
            continue
        row = dict(info)
        row["copyright_path"] = license_file(info)
        row["source_command"] = "apt-get source '{}={}'".format(info["source_package"], info["source_version"])
        package_rows.append(row)
    common = Path("/usr/share/common-licenses")
    for source in sorted(common.iterdir()):
        if source.is_file():
            if not inside(source.resolve(strict=True), common):
                fail("common-license ссылается вне каталога: {}".format(source))
            plain_copy(source, stage / "licenses/runtime/common-licenses" / source.name)
    for needed in ("LGPL-2.1", "LGPL-3", "GPL-2", "GPL-3"):
        if not (stage / "licenses/runtime/common-licenses" / needed).is_file():
            fail("Не найден обязательный текст лицензии {}".format(needed))
    tsv(stage / "runtime/MANIFEST.tsv", [manifest[name] for name in sorted(manifest)],
        ("path", "kind", "binary_package", "binary_version", "source_package", "source_version", "source_path", "resolved_source_path", "sha256", "link_target"))
    tsv(stage / "runtime/SYSTEM-LIBRARIES.tsv", [system_manifest[key] for key in sorted(system_manifest)],
        ("soname", "binary_package", "binary_version", "source_package", "source_version", "source_path", "resolved_source_path", "reason", "verification",
         "minimum_tested_version", "minimum_tested_profile", "minimum_tested_evidence", "minimum_profile_privileged_validation"))
    tsv(stage / "licenses/runtime/PACKAGES.tsv", package_rows,
        ("binary_package", "binary_version", "source_package", "source_version", "copyright_path", "source_command"))

    text(stage / "runtime/README.txt", """Runtime Зарницы {version}, Astra Linux 1.7 x86_64

Это {completeness}. Qt {qt} загружается динамически. Манифест MANIFEST.tsv
указывает исходный dpkg-пакет, версию, Source/Version, исходный путь и SHA-256
каждой библиотеки, утилиты, plugin и таблицы xkb. Символьные ссылки относительные.
SYSTEM-LIBRARIES.tsv перечисляет намеренно НЕ включённую системную glibc
и external base Astra libgost/libparsec-base3/libpdp с версиями и доказательством
их наличия в bare image (metadata dpkg-query передаётся с хоста). Копии слоёв
системной MAC/policy-защиты не подменяют рабочую Astra. binary_version и verification
в этой таблице относятся к фактическому build/bare образу, НЕ к нижнему порогу.
Минимумы .deb для версии 1.0.0 подтверждены пользовательским forced-bundled smoke
на живой Astra SE 1.7.6: libgost >= 2.0.2-5+ci4, libparsec-base3/libpdp >= 3.4+ci71.
Свидетельство и минимум каждого пакета — отдельные minimum_tested_* поля.
Проверки с sudo, реальных МКЦ/file capabilities на этой Astra 1.7.6 НЕ было;
успешная выдача capability в ubi17-контейнере не подтверждает политику МКЦ хоста.

Системная основа: glibc 2.28 Astra, ELF loader, базовые shell/coreutils.
libc/libpthread/libm/libdl/librt/libresolv/libnss/libcrypt и loader НЕ копируются.
Есть libstdc++, libgcc, XCB/xkb, Qt platform plugins xcb/offscreen/minimal,
а также доступные imageformats, bearer, xcbglintegrations и input contexts.
Для bearer/IBus при их наличии нужен системный DBus; демон DBus не запускается
и конфигурация сессии/секреты не переносятся. X11 server, видеодрайвер и системное
хранилище доверенных CA остаются системными; Mesa/DRI-драйверы не подменяются.
libssl.so.1.1 и libcrypto.so.1.1 добавлены явно, так как Qt Network делает dlopen.

Прямой диагностический запуск из корня комплекта (обычно нужен zarnitsa.sh):
  LD_LIBRARY_PATH="$PWD/runtime/lib" QT_PLUGIN_PATH="$PWD/runtime/plugins" \\
  XKB_CONFIG_ROOT="$PWD/runtime/share/X11/xkb" \\
  FONTCONFIG_FILE="$PWD/runtime/etc/fonts/fonts.conf" QT_QPA_PLATFORM=offscreen \\
  ./bin/zarnitsa --demo --screenshot /tmp/zarnitsa.png

runtime/bin/ping, openssl, setcap, getcap — plain 0755, без setuid/capabilities.
Им нужен LD_LIBRARY_PATH, указывающий на runtime/lib. openssl для локального
сертификата использует отдельный минимальный req-конфиг; пустой /dev/null
не подходит для OpenSSL 1.1.1. Конфиги/ключи сборочной машины не включены. Без cap_net_raw при ping_group_range='1 0' переносной ping не
обязан работать: штатный root install/configure выдаёт capability только
проверенному статическому bin/zarnitsa-icmp. sysctl автоматически не меняется.

Замена Qt: бинарник bin/zarnitsa не содержит встроенного пути к bundled Qt.
Его можно запустить напрямую с совместимыми системными Qt 5.15 и plugins,
убрав LD_LIBRARY_PATH/QT_PLUGIN_PATH из окружения. Не смешивайте несовместимые
версии Qt и plugins. Исходники, лицензии и точные версии — в licenses/runtime,
THIRD-PARTY-NOTICES.txt и source/. Исходники Qt автоматически не скачиваются.
Комплект не содержит build dependencies и НЕ обещает полностью офлайн или
побитово воспроизводимую пересборку. Это не заявление о сертификации релиза.
""".format(version=VERSION, qt=QT_VERSION, completeness="промежуточный runtime-only stage, НЕ готовый установочный релиз" if RUNTIME_ONLY else "полный установочный payload"))

    source_sha = "не создавался (runtime-only)"
    source_tree_sha = "не снимался (runtime-only --reuse-build)" if REUSE_BUILD else source_fingerprint(source_paths())
    if not RUNTIME_ONLY:
        paths = source_paths()
        source_archive = stage / "source" / ("zarnitsa-" + VERSION + ".tar.gz")
        archive(source_archive, [(SOURCE, "zarnitsa-" + VERSION)] + [(p, "zarnitsa-" + VERSION + "/" + p.relative_to(SOURCE).as_posix()) for p in paths], SOURCE)
        source_sha = sha(source_archive)
        # Наши .o позволяют перелинковать статический helper другим libc/toolchain.
        object_root = BUILD / "CMakeFiles/zarnitsa-icmp.dir"
        objects = sorted(object_root.rglob("*.o"))
        if not objects:
            fail("Не найдены объектные файлы для перелинковки статического helper")
        for source in objects:
            plain_copy(source, stage / "source/relink-zarnitsa-icmp" / source.relative_to(object_root))
        text(stage / "source/README.txt", """В zarnitsa-{version}.tar.gz — рабочее дерево исходников программы, включая
несохранённые в git правки, тесты, ресурсы и скрипты; без .git/dist/build/home.
Лицензия нашей программы — LICENSE в комплекте, если этот файл предоставлен
автором. Архив сам по себе не переводит программу в свободную лицензию.

Build dependencies (C++17, CMake/Ninja, Qt5 dev и пр.) НЕ включены. Для повторной
сборки можно использовать описанный в BUILD.txt образ и docker/Dockerfile.astra17;
зависимости и базовый образ придётся иметь заранее или получить отдельно.
Ни полностью офлайн-пересборка, ни битовая воспроизводимость не обещаются.

Для LGPL-перелинковки статического ICMP helper сохранены НАШИ объектные файлы
в relink-zarnitsa-icmp/. Пример из этого каталога с совместимым g++:
  g++ -static src/icmp/helper_main.cpp.o src/icmp/icmp_packet.cpp.o -o zarnitsa-icmp
Статические glibc/libstdc++/libgcc и toolchain не включены. Их Source/Version
и notices отражены в licenses/runtime/PACKAGES.tsv. Исходники и патчи библиотек
получаются отдельно из соответствующих официальных исходных пакетов Astra.
Qt связан динамически; официальный upstream source — в THIRD-PARTY-NOTICES.txt.
""".format(version=VERSION))

    for name, previous in original_binary_sha.items():
        if sha(BUILD / name) != previous:
            fail("Исходный build binary изменился во время упаковки: {}".format(name))
    if not REUSE_BUILD and source_fingerprint(source_paths(), code_only=True) != code_before:
        fail("Код изменился во время упаковки; публикация отменена")
    lines = ["Зарница — протокол сборки офлайн-комплекта (НЕ сертификат релиза)",
             "VERSION=" + VERSION, "REVISION=" + REVISION, "BUILD_DATE=" + BUILD_DATE,
             "PACKAGE_MODE=" + ("runtime-only-incomplete" if RUNTIME_ONLY else "full"),
             "SOURCE_COMMIT=" + COMMIT, "GIT_DIRTY=" + DIRTY,
             "GIT_METADATA_SOURCE=передано вызывающей стороной; git проверяется дополнительно, если доступен",
             "SOURCE_TREE_SHA256=" + source_tree_sha, "SOURCE_ARCHIVE_SHA256=" + source_sha,
             "DOCKER_IMAGE_DIGEST=" + IMAGE_DIGEST, "DOCKER_IMAGE_ID=" + IMAGE_ID,
             "BASE_DOCKER_IMAGE_DIGEST=" + BASE_DIGEST,
             "EXTERNAL_BASE_BUILD_AND_BARE_VERSIONS=" + ";".join("{}={}".format(name, version) for name, version in sorted(BASE_PACKAGE_VERSIONS.items())),
             "EXTERNAL_BASE_VERIFICATION=фактический dpkg-query из bare image, передан хостом и сверен с builder; SYSTEM-LIBRARIES.tsv; это НЕ минимумы Depends",
             "EXTERNAL_BASE_MINIMUM_TESTED_VERSIONS=" + ";".join("{}={}".format(name, version) for name, version in sorted(TESTED_BASE_MINIMUM_VERSIONS.items())),
             "EXTERNAL_BASE_MINIMUM_TESTED_PROFILE=" + TESTED_BASE_MINIMUM_PROFILE,
             "EXTERNAL_BASE_MINIMUM_TESTED_EVIDENCE=" + TESTED_BASE_MINIMUM_EVIDENCE,
             "EXTERNAL_BASE_MINIMUM_PROFILE_PRIVILEGED_VALIDATION=" + TESTED_BASE_PRIVILEGED_VALIDATION,
             "DOCKER_METADATA_SOURCE=immutable reference из docker image inspect на хосте",
             "TARGET=astra1.7_x86_64", "GLIBC=" + output("getconf", "GNU_LIBC_VERSION"),
             "GLIBC_PACKAGE=" + output("dpkg-query", "-W", "-f=${Version}", "libc6"),
             "QT=" + QT_VERSION,
             "QT_PACKAGE=" + output("dpkg-query", "-W", "-f=${Version}", "libqt5core5a"),
             "CXX_COMPILER=" + COMPILER, "CXX_VERSION=" + output(COMPILER, "--version").splitlines()[0],
             "CXX_TARGET=" + output(COMPILER, "-dumpmachine"),
             "CMAKE=" + output("cmake", "--version").splitlines()[0],
             "NINJA=" + output("ninja", "--version"), "BUILD_TYPE=Release; C++17; GUI=ON; ZARNITSA_TESTS=ON",
             "CMAKE_C_FLAGS=" + cache.get("CMAKE_C_FLAGS", ""),
             "CMAKE_C_FLAGS_RELEASE=" + cache.get("CMAKE_C_FLAGS_RELEASE", ""),
             "CMAKE_CXX_FLAGS=" + cache.get("CMAKE_CXX_FLAGS", ""),
             "CMAKE_CXX_FLAGS_RELEASE=" + cache.get("CMAKE_CXX_FLAGS_RELEASE", ""),
             "CMAKE_EXE_LINKER_FLAGS=" + cache.get("CMAKE_EXE_LINKER_FLAGS", ""),
             "CTEST=" + ("NOT_RUN (runtime-only --reuse-build)" if REUSE_BUILD else "PASSED; ctest --output-on-failure (без pipeline)"),
             "SOURCE_BINARY_MATCH=" + ("NOT_VERIFIED (runtime-only --reuse-build)" if REUSE_BUILD else "сборка текущего source tree проверена"),
             "GLIBC_RUNTIME=system; не включена в runtime/lib; helper статический",
             "LICENSE_NOTICES=" + ("INCOMPLETE: " + ", ".join(missing_licenses) if missing_licenses else "copyright и common-license тексты скопированы"),
             "REPRODUCIBLE_BUILD=не заявляется; build dependencies и источники сторонних библиотек не включены",
             "CERTIFICATION=не заявляется"]
    for name in sorted(original_binary_sha):
        lines.append("BUILD_BINARY_SHA256[{}]={}".format(name, original_binary_sha[name]))
        lines.append("PACKAGED_BINARY_SHA256[{}]={}".format(name, sha(stage / "bin" / name)))
    text(stage / "BUILD.txt", "\n".join(lines) + "\n")
    if RUNTIME_ONLY:
        text(stage / "RUNTIME_ONLY", "Промежуточный stage: полный архив, source archive и .deb ещё не выпускались.\n")
    checksums(stage)
    if not RUNTIME_ONLY:
        run(["bash", "-euo", "pipefail", "-c",
             'source "$1/zarnitsa-common.sh"; ZR_BASE=$1; zr_verify_bundle "$1"', "package-verify", stage])

    # Архивы сначала строятся во временном каталоге dist; неполный файл не выдаётся за готовый.
    artifacts = []
    if not RUNTIME_ONLY:
        DIST.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=".zarnitsa-package-", dir=str(DIST)) as temporary:
            temporary = Path(temporary)
            stem = "Zarnitsa_{}_offline_astra1.7_x86_64_{}".format(VERSION, REVISION)
            tar_path = temporary / (stem + ".tar.gz")
            archive(tar_path, [(stage, "Zarnitsa")] + [(p, "Zarnitsa/" + p.relative_to(stage).as_posix()) for p in all_paths(stage)], stage)
            deb_path = temporary / (stem + ".deb")
            make_deb(temporary, deb_path)
            for path in (tar_path, deb_path):
                target = DIST / path.name
                os.replace(str(path), str(target))
                artifacts.append(target)

    if STAGE.is_symlink():
        fail("Итоговый stage не должен быть символьной ссылкой")
    previous = PACKAGE / ".Zarnitsa-previous"
    if previous.exists() or previous.is_symlink():
        fail("Остался предыдущий backup stage; проверьте {} вручную".format(previous))
    if STAGE.exists():
        os.replace(str(STAGE), str(previous))
    try:
        os.replace(str(stage), str(STAGE))
    except BaseException:
        if previous.exists():
            os.replace(str(previous), str(STAGE))
        raise
    if previous.exists():
        shutil.rmtree(str(previous))
    print("\nГотов stage: {}".format(STAGE), flush=True)
    print("Режим: {}; runtime ELF/plugins/data: {}; dpkg packages: {}".format("runtime-only (НЕ релиз)" if RUNTIME_ONLY else "full", len(manifest), len(package_rows)), flush=True)
    if missing_licenses:
        print("НЕ ГОТОВО К РАСПРОСТРАНЕНИЮ: отсутствуют copyrights: " + ", ".join(missing_licenses), flush=True)
    for directory in ("bin", "runtime", "licenses", "source"):
        root = STAGE / directory
        size = sum(p.stat().st_size for p in all_paths(root) if p.is_file() and not p.is_symlink()) if root.exists() else 0
        print("{}: {:.2f} MiB".format(directory, size / 1024.0 ** 2), flush=True)
    for path in artifacts:
        print("{}  {}  ({:.2f} MiB)".format(sha(path), path, path.stat().st_size / 1024.0 ** 2), flush=True)
finally:
    if stage.exists() and stage != STAGE:
        shutil.rmtree(str(stage))
PY
