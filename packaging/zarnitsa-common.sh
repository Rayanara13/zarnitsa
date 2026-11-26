#!/bin/bash
# Общие проверки комплекта. VERSION.txt и маркеры никогда не исполняются как shell.
# Библиотека поставляется рядом со всеми скриптами в корне Zarnitsa/.

[[ -n ${ZARNITSA_COMMON_LOADED:-} ]] && return 0
ZARNITSA_COMMON_LOADED=1

# Привилегированные служебные команды не ищем в пользовательском PATH.
if (( EUID == 0 )); then
    export PATH=/usr/sbin:/usr/bin:/sbin:/bin
fi

zr_error() { printf 'Ошибка: %s\n' "$*" >&2; }
zr_warn() { printf 'Предупреждение: %s\n' "$*" >&2; }
zr_die() { zr_error "$*"; exit 1; }
zr_need() { command -v "$1" >/dev/null 2>&1 || zr_die "Не найдена необходимая команда: $1"; }

zr_plain_path() {
    [[ $1 == /* && $1 != *$'\n'* && $1 != *$'\r'* && $1 != *'\'* && $1 != *$'\t'* ]]
}

zr_marker() {
    [[ -f $1 && ! -L $1 ]] && grep -Fxq -- "$2" "$1"
}

zr_is_deb() {
    [[ -e $1/DEBIAN_PACKAGE || -L $1/DEBIAN_PACKAGE || $1 == /opt/zarnitsa-deb || $1 == /opt/zarnitsa-deb/* ]]
}

zr_read_version() {
    local base=$1 line seen_version=0 seen_revision=0
    ZR_VERSION= ZR_REVISION= ZR_RELEASE_ID=
    [[ -f $base/VERSION.txt && ! -L $base/VERSION.txt ]] || { zr_error "Нет обычного файла $base/VERSION.txt"; return 1; }
    while IFS= read -r line || [[ -n $line ]]; do
        line=${line%$'\r'}
        case $line in
            ''|'#'*) ;;
            VERSION=*)
                (( seen_version == 0 )) || { zr_error 'В VERSION.txt повторено VERSION'; return 1; }
                ZR_VERSION=${line#VERSION=}; seen_version=1 ;;
            REVISION=*)
                (( seen_revision == 0 )) || { zr_error 'В VERSION.txt повторено REVISION'; return 1; }
                ZR_REVISION=${line#REVISION=}; seen_revision=1 ;;
            BUILD_DATE=*) ;; # Необязательные метаданные, не команда и не источник переменных.
            *) zr_error 'Неизвестная строка VERSION.txt; нужны VERSION=… и REVISION=…'; return 1 ;;
        esac
    done < "$base/VERSION.txt"
    [[ $ZR_VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z][0-9A-Za-z.-]*)?(\+[0-9A-Za-z][0-9A-Za-z.-]*)?$ ]] || {
        zr_error 'Недопустимый VERSION; ожидается, например, 1.0.0'; return 1;
    }
    [[ $ZR_REVISION =~ ^[0-9A-Za-z][0-9A-Za-z._-]{0,63}$ ]] || {
        zr_error 'Недопустимый REVISION; нужны 1–64 символа: латиница, цифры, точка, дефис, подчёркивание'; return 1;
    }
    ZR_RELEASE_ID=$ZR_VERSION-$ZR_REVISION
}

zr_safe_member() {
    [[ -n $1 && $1 != /* && $1 != *'\'* && $1 != *$'\n'* && $1 != *$'\r'* && $1 != *$'\t'* ]] || return 1
    case /$1/ in */../*|*/./*|*//* ) return 1 ;; esac
}

zr_manifest_digest() {
    local value
    value=$(sha256sum -- "$1/SHA256SUMS") || return 1
    printf '%s\n' "${value%% *}"
}

zr_verify_bundle() {
    local base=$1 line name path resolved link digest count=0
    local -A listed=()
    ZR_VERIFIED_BASE=
    [[ -d $base && ! -L $base && -f $base/SHA256SUMS && ! -L $base/SHA256SUMS ]] || {
        zr_error "Нет обычного SHA256SUMS в $base"; return 1;
    }
    zr_plain_path "$base" || { zr_error 'Неподдерживаемые управляющие символы в пути комплекта'; return 1; }
    zr_read_version "$base" || return 1
    while IFS= read -r line || [[ -n $line ]]; do
        [[ $line =~ ^([0-9a-fA-F]{64})\ ([\ *])(.+)$ ]] || {
            zr_error 'Некорректная строка SHA256SUMS (нужен обычный формат sha256sum)'; return 1;
        }
        name=${BASH_REMATCH[3]}; name=${name#./}
        zr_safe_member "$name" || { zr_error "Небезопасный путь в SHA256SUMS: $name"; return 1; }
        [[ $name != SHA256SUMS && $name != .zarnitsa-release && -z ${listed[$name]+yes} ]] || {
            zr_error "Повторённый или служебный путь в SHA256SUMS: $name"; return 1;
        }
        [[ -f $base/$name ]] || { zr_error "Нет файла из SHA256SUMS: $name"; return 1; }
        listed["$name"]=1
        count=$((count + 1))
    done < "$base/SHA256SUMS"
    (( count > 0 )) || { zr_error 'SHA256SUMS пуст'; return 1; }
    for name in VERSION.txt BUILD.txt ИНСТРУКЦИЯ.txt bin/zarnitsa bin/zarnitsa-icmp \
        zarnitsa.sh zarnitsa-common.sh install.sh uninstall.sh configure-icmp.sh \
        check-runtime.sh diagnose.sh make-cert.sh zarnitsa.desktop.in; do
        [[ -n ${listed[$name]+yes} && ! -L $base/$name ]] || {
            zr_error "Обязательный обычный файл не покрыт SHA256SUMS: $name"; return 1;
        }
    done
    for name in bin runtime runtime/lib runtime/plugins runtime/plugins/platforms icons licenses; do
        [[ -d $base/$name && ! -L $base/$name ]] || { zr_error "Нет обычного каталога $name"; return 1; }
    done
    [[ -f $base/runtime/plugins/platforms/libqxcb.so ]] || { zr_error 'В комплекте нет платформенного плагина xcb'; return 1; }
    find "$base" -xdev -print >/dev/null || { zr_error 'Не удалось обойти весь комплект'; return 1; }
    while IFS= read -r -d '' path; do
        name=${path#"$base/"}
        zr_safe_member "$name" || { zr_error 'В комплекте есть небезопасное имя файла'; return 1; }
        if [[ -L $path ]]; then
            # Ссылки библиотек допускаются лишь внутри runtime и только на содержимое комплекта.
            link=$(readlink -- "$path") || return 1
            [[ $name == runtime/* && $link != /* && $link != *$'\n'* && $link != *'\'* ]] || {
                zr_error "Недопустимая символическая ссылка: $name"; return 1;
            }
            resolved=$(readlink -f -- "$path") || { zr_error "Повреждённая ссылка: $name"; return 1; }
            [[ $resolved == "$base/"* && -e $resolved ]] || { zr_error "Ссылка выходит из комплекта: $name"; return 1; }
        elif [[ -f $path ]]; then
            case $name in
                SHA256SUMS) ;;
                .zarnitsa-release)
                    zr_marker "$path" 'ZARNITSA_RELEASE=1' && zr_marker "$path" "ID=$ZR_RELEASE_ID" || {
                        zr_error 'Неверный маркер установленного релиза'; return 1;
                    }
                    digest=$(zr_manifest_digest "$base") || return 1
                    zr_marker "$path" "MANIFEST_SHA256=$digest" || { zr_error 'SHA256SUMS отличается от проверенного при установке'; return 1; } ;;
                *) [[ -n ${listed[$name]+yes} ]] || { zr_error "Посторонний файл не покрыт SHA256SUMS: $name"; return 1; } ;;
            esac
        elif [[ ! -d $path ]]; then
            zr_error "Специальный файл запрещён: $name"; return 1
        fi
    done < <(find "$base" -xdev -mindepth 1 -print0)
    # Проверка не выполняет бинарники и проводится до копирования, затем ещё раз на копии.
    ( cd -- "$base" && LC_ALL=C sha256sum --check --strict --status SHA256SUMS ) || {
        zr_error "Контрольные суммы не совпали: $base"; return 1;
    }
    ZR_VERIFIED_BASE=$base
}

zr_secure_path() {
    local path=$1 uid mode
    [[ -e $path && ! -L $path ]] || { zr_error "Ожидался существующий путь без ссылки: $path"; return 1; }
    path=$(readlink -f -- "$path") || return 1
    while :; do
        uid=$(stat -c '%u' -- "$path") || return 1
        mode=$(stat -c '%a' -- "$path") || return 1
        [[ $uid == 0 ]] && (( (8#$mode & 0022) == 0 )) || {
            zr_error "Путь должен принадлежать root и быть недоступен для записи группе/остальным: $path"; return 1;
        }
        [[ $path == / ]] && break
        path=${path%/*}; [[ -n $path ]] || path=/
    done
}

zr_secure_tree() {
    local base=$1 bad
    zr_secure_path "$base" || return 1
    bad=$(find "$base" -xdev \( ! -user root -o \( ! -type l -perm /022 \) \) -print -quit) || return 1
    [[ -z $bad ]] || { zr_error "Незащищённый объект в установленной программе: $bad"; return 1; }
}

zr_native_tool() {
    local name=$1 dir
    for dir in /usr/sbin /usr/bin /sbin /bin; do
        if [[ -x $dir/$name && ! -d $dir/$name ]]; then
            printf '%s\n' "$dir/$name"; return 0
        fi
    done
    return 1
}

zr_tool_command() {
    local name=$1 native base=${ZR_BASE:?} resolved
    ZR_TOOL_COMMAND=()
    case $name in openssl|setcap|getcap|ping) ;; *) return 1 ;; esac
    if native=$(zr_native_tool "$name"); then
        ZR_TOOL_COMMAND=(env -u LD_LIBRARY_PATH -u LD_PRELOAD -u LD_AUDIT -u OPENSSL_MODULES -u OPENSSL_ENGINES OPENSSL_CONF=/dev/null "$native")
        return 0
    fi
    [[ -x $base/runtime/bin/$name && ! -L $base/runtime/bin/$name ]] || return 1
    # Никакого добавления переносного runtime в root PATH. Fallback — только проверенный.
    if (( EUID == 0 )); then zr_secure_tree "$base" || return 1; fi
    if [[ ${ZR_VERIFIED_BASE:-} != "$base" ]]; then zr_verify_bundle "$base" || return 1; fi
    resolved=$(readlink -f -- "$base/runtime/bin/$name") || return 1
    [[ $resolved == "$base/runtime/bin/$name" ]] || return 1
    ZR_TOOL_COMMAND=(env -u LD_PRELOAD -u LD_AUDIT -u OPENSSL_MODULES -u OPENSSL_ENGINES LD_LIBRARY_PATH="$base/runtime/lib" OPENSSL_CONF=/dev/null "$resolved")
}

zr_caps() {
    zr_tool_command getcap || return 1
    "${ZR_TOOL_COMMAND[@]}" "$1"
}

zr_static_helper() {
    local file=$1 magic output
    [[ -f $file && ! -L $file && -x $file ]] || { zr_error 'ICMP-помощник отсутствует, является ссылкой или не исполняемый'; return 1; }
    magic=$(od -An -tx1 -N4 -- "$file") || return 1
    [[ ${magic//[[:space:]]/} == 7f454c46 ]] || { zr_error 'ICMP-помощник не является ELF'; return 1; }
    output=$(env -u LD_LIBRARY_PATH -u LD_PRELOAD -u LD_AUDIT LC_ALL=C ldd "$file" 2>&1) || :
    [[ $output == *'not a dynamic executable'* || $output == *'statically linked'* || $output == *'not dynamic executable'* ]] || {
        zr_error 'ICMP-помощник обязан быть статическим, без Qt и загрузки shared libraries'; return 1;
    }
}

zr_default_paths() {
    ZR_SCOPE=$1
    ZR_DATA_HOME=${XDG_DATA_HOME:-${HOME:?}/.local/share}
    ZR_CONFIG_HOME=${XDG_CONFIG_HOME:-$HOME/.config}
    zr_plain_path "$ZR_DATA_HOME" && zr_plain_path "$ZR_CONFIG_HOME" && zr_plain_path "$HOME" || {
        zr_error 'HOME, XDG_DATA_HOME и XDG_CONFIG_HOME должны быть абсолютными путями без управляющих символов'; return 1;
    }
    if [[ $ZR_SCOPE == system ]]; then
        ZR_PREFIX=/opt/zarnitsa
        ZR_BIN=/usr/local/bin/zarnitsa
        ZR_DESKTOP=/usr/local/share/applications/zarnitsa.desktop
        ZR_ICONS=/usr/local/share/icons/hicolor
        ZR_UNIT=/etc/systemd/system/zarnitsa.service
    else
        ZR_PREFIX=$ZR_DATA_HOME/zarnitsa-program
        ZR_BIN=$HOME/.local/bin/zarnitsa
        ZR_DESKTOP=$ZR_DATA_HOME/applications/zarnitsa.desktop
        ZR_ICONS=$ZR_DATA_HOME/icons/hicolor
        ZR_UNIT=$ZR_CONFIG_HOME/systemd/user/zarnitsa.service
    fi
}

zr_installation_marker() {
    zr_marker "$1/.zarnitsa-installation" 'ZARNITSA_INSTALLATION=1' &&
        zr_marker "$1/.zarnitsa-installation" "SCOPE=$ZR_SCOPE" &&
        zr_marker "$1/.zarnitsa-installation" "OWNER_UID=$EUID"
}

zr_check_installed_release() {
    local prefix=$1 target=$2 release
    [[ $target == releases/* && $target != */*/* ]] || { zr_error 'Неверная цель ссылки релиза'; return 1; }
    release=$prefix/$target
    [[ -d $release && ! -L $release ]] || { zr_error "Релиз не является обычным каталогом: $release"; return 1; }
    zr_read_version "$release" || return 1
    [[ $target == "releases/$ZR_RELEASE_ID" ]] && zr_marker "$release/.zarnitsa-release" 'ZARNITSA_RELEASE=1' || {
        zr_error 'Имя релиза не соответствует VERSION.txt/маркеру'; return 1;
    }
    zr_verify_bundle "$release"
}

zr_owned_desktop() {
    zr_marker "$ZR_DESKTOP" 'X-Zarnitsa-Managed=true' && zr_marker "$ZR_DESKTOP" "X-Zarnitsa-InstallRoot=$ZR_PREFIX"
}

zr_owned_unit() {
    zr_marker "$ZR_UNIT" '# ZARNITSA_MANAGED_UNIT=1' && zr_marker "$ZR_UNIT" "# ZARNITSA_INSTALL_ROOT=$ZR_PREFIX"
}

zr_owned_icon() {
    local dest=$1 link name
    [[ -L $dest ]] || return 1
    link=$(readlink -- "$dest") || return 1
    name=${link#"$ZR_PREFIX/current/icons/"}
    [[ $link != "$name" && $name =~ ^[0-9]+x[0-9]+/apps/zarnitsa\.png$ ]]
}

zr_confirm() {
    local text=$1 answer
    [[ -t 0 ]] || { zr_error 'Нужен терминал для подтверждения; для согласованного unattended-запуска передайте --yes'; return 2; }
    printf '%s [да/НЕТ] ' "$text" >&2
    IFS= read -r answer || return 2
    [[ $answer == да || $answer == ДА || $answer == yes ]]
}

zr_systemd_quote() {
    local value=$1
    value=${value//\\/\\\\}; value=${value//\"/\\\"}
    value=${value//\$/\$\$}; value=${value//%/%%}
    printf '"%s"' "$value"
}

zr_desktop_exec_quote() {
    local value=$1
    value=${value//\\/\\\\\\\\}; value=${value//\"/\\\\\\\"}
    value=${value//\$/\\\\\$}; value=${value//\`/\\\\\`}; value=${value//%/%%}
    printf '"%s"' "$value"
}

# Проверка динамических зависимостей не запускает само приложение, не читает его конфиг.
zr_ldd() {
    local file=$1 libs=$2 output rc=0
    output=$(env -u LD_PRELOAD -u LD_AUDIT LD_LIBRARY_PATH="$libs" LC_ALL=C ldd -r "$file" 2>&1) || rc=$?
    ZR_LDD_OUTPUT=$output
    ZR_RUNTIME_REPORT+=$'\n'"ldd -r $file:"$'\n'"$output"$'\n'
    (( rc == 0 )) && ! grep -Eiq 'not found|undefined symbol' <<< "$output"
}

zr_qt_core_from_ldd() {
    local output=$1 line path core= resolved
    while IFS= read -r line; do
        if [[ $line =~ libQt5[^[:space:]]*[[:space:]]+\=\>[[:space:]]+(/.+)[[:space:]]+\(0x ]]; then
            path=${BASH_REMATCH[1]}
            resolved=$(readlink -f -- "$path") || return 1
            if [[ $ZR_PROBE_KIND == system ]]; then
                [[ $resolved != "$ZR_BASE/runtime/"* && ( $resolved == /usr/* || $resolved == /lib/* || $resolved == /lib64/* ) ]] || {
                    ZR_RUNTIME_REPORT+=$'\n''Системная проверка обнаружила Qt из несистемного каталога.'; return 1;
                }
            else
                [[ $resolved == "$ZR_BASE/runtime/lib/"* ]] || {
                    ZR_RUNTIME_REPORT+=$'\n''Обнаружено смешивание комплектного и системного Qt.'; return 1;
                }
            fi
            [[ $line == *libQt5Core.so* ]] && core=$resolved
        fi
    done <<< "$output"
    [[ ${core##*/} =~ ^libQt5Core\.so\.5\.15\.[0-9]+([.-].*)?$ ]] || {
        ZR_RUNTIME_REPORT+=$'\n''Нужен Qt 5.15.x; Qt 5.11 и неизвестные версии не подходят.'; return 1;
    }
    ZR_QT_CORE=$core
}

zr_probe_runtime() {
    local kind=$1 platform=$2 base=${ZR_BASE:?} libs core root plugin candidate
    local -a roots=()
    ZR_RUNTIME_REPORT= ZR_QT_CORE= ZR_PROBE_PLUGIN_ROOT= ZR_PROBE_LIBS=
    ZR_PROBE_KIND=$kind
    [[ -x $base/bin/zarnitsa && ! -L $base/bin/zarnitsa ]] || {
        ZR_RUNTIME_REPORT='Нет исполняемого обычного bin/zarnitsa'; return 1;
    }
    if [[ $kind == system ]]; then
        # Явный список не даёт пользовательскому LD_LIBRARY_PATH подмешать другой Qt.
        libs=/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu:/usr/lib64:/lib64:/usr/lib:/lib
    else
        [[ -d $base/runtime/lib && -d $base/runtime/plugins ]] || {
            ZR_RUNTIME_REPORT='Нет runtime/lib или runtime/plugins'; return 1;
        }
        libs=$base/runtime/lib
    fi
    zr_ldd "$base/bin/zarnitsa" "$libs" || { ZR_RUNTIME_REPORT+=$'\n''Зависимости приложения не разрешены.'; return 1; }
    zr_qt_core_from_ldd "$ZR_LDD_OUTPUT" || return 1
    core=$ZR_QT_CORE
    if [[ $kind == bundled ]]; then
        roots=("$base/runtime/plugins")
    else
        roots=("${core%/*}/qt5/plugins" /usr/lib/x86_64-linux-gnu/qt5/plugins /usr/lib/qt5/plugins /usr/lib64/qt5/plugins /usr/lib/x86_64-linux-gnu/qt/plugins)
    fi
    # QCoreApplication не загружает QPA: headless не требует xcb/X11 даже на сервере.
    if [[ $platform == headless ]]; then
        ZR_PROBE_PLUGIN_ROOT=${roots[0]}; ZR_PROBE_LIBS=$libs
        return 0
    fi
    for candidate in "${roots[@]}"; do
        [[ -d $candidate ]] || continue
        root=$(readlink -f -- "$candidate") || continue
        plugin=$root/platforms/libq${platform}.so
        [[ -f $plugin ]] || continue
        if zr_ldd "$plugin" "$libs" && zr_qt_core_from_ldd "$ZR_LDD_OUTPUT" && [[ $ZR_QT_CORE == "$core" ]]; then
            ZR_PROBE_PLUGIN_ROOT=$root; ZR_PROBE_LIBS=$libs
            return 0
        fi
    done
    ZR_RUNTIME_REPORT+=$'\n'"Не найден совместимый платформенный плагин $platform (с тем же Qt 5.15)."
    return 1
}

zr_select_runtime() {
    local request=$1 platform=$2
    ZR_SELECTED_RUNTIME= ZR_SELECTED_LIBS= ZR_SELECTED_PLUGINS= ZR_SYSTEM_REPORT= ZR_BUNDLED_REPORT=
    case $request in auto|system|bundled) ;; *) zr_error 'ZARNITSA_QT_RUNTIME/--qt-runtime: допустимы auto, system, bundled'; return 1 ;; esac
    if [[ $request != bundled ]]; then
        if zr_probe_runtime system "$platform"; then
            ZR_SELECTED_RUNTIME=system; ZR_SELECTED_LIBS=$ZR_PROBE_LIBS; ZR_SELECTED_PLUGINS=$ZR_PROBE_PLUGIN_ROOT
            ZR_SYSTEM_REPORT=$ZR_RUNTIME_REPORT
            return 0
        fi
        ZR_SYSTEM_REPORT=$ZR_RUNTIME_REPORT
        [[ $request == auto ]] || return 1
    fi
    if zr_probe_runtime bundled "$platform"; then
        ZR_SELECTED_RUNTIME=bundled; ZR_SELECTED_LIBS=$ZR_PROBE_LIBS; ZR_SELECTED_PLUGINS=$ZR_PROBE_PLUGIN_ROOT
        ZR_BUNDLED_REPORT=$ZR_RUNTIME_REPORT
        return 0
    fi
    ZR_BUNDLED_REPORT=$ZR_RUNTIME_REPORT
    return 1
}

zr_print_icmp() {
    local helper=$ZR_BASE/bin/zarnitsa-icmp ping caps
    printf 'Помощник ICMP: %s\n' "$helper"
    if [[ -f $helper ]]; then
        stat -c '  Владелец UID:GID %u:%g; режим %a; жёстких ссылок %h' -- "$helper" || :
        if caps=$(zr_caps "$helper" 2>/dev/null); then printf '  Capabilities: %s\n' "${caps:-нет}"; else printf '  getcap недоступен (права не менялись).\n'; fi
        if [[ -u $helper || -g $helper ]]; then zr_warn 'Помощник имеет setuid/setgid; такой комплект нельзя использовать'; fi
    else
        printf '  Файл отсутствует.\n'
    fi
    if ping=$(zr_native_tool ping); then
        printf 'Системный ping: %s\n' "$ping"
        stat -Lc '  Владелец UID:GID %u:%g; режим %a' -- "$ping" || :
        if caps=$(zr_caps "$ping" 2>/dev/null); then printf '  Capabilities ping: %s\n' "${caps:-нет}"; fi
    else
        printf 'Системный /usr/bin/ping не найден; проверьте runtime/bin/ping и доступные ICMP-режимы.\n'
    fi
    printf 'ping_group_range (только чтение): '
    if [[ -r /proc/sys/net/ipv4/ping_group_range ]]; then
        local range
        IFS= read -r range < /proc/sys/net/ipv4/ping_group_range || :
        printf '%s\n' "$range"
    else
        printf 'недоступен\n'
    fi
}
