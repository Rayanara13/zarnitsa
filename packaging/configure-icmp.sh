#!/bin/bash
# Только cap_net_raw для статического помощника. Никаких setuid, sysctl и firewall.
set -euo pipefail
SELF=$(readlink -f -- "${BASH_SOURCE[0]}")
BASE=${SELF%/*}
source "$BASE/zarnitsa-common.sh"
ZR_BASE=$BASE

action=diagnose
while (( $# )); do
    case $1 in
        --enable|--disable)
            [[ $action == diagnose ]] || zr_die 'Выберите только один из --enable/--disable'
            action=${1#--} ;;
        -h|--help)
            printf '%s\n' 'configure-icmp.sh — диагностика ICMP без изменений' \
                '  --enable   root: cap_net_raw+ep только защищённому установленному bin/zarnitsa-icmp' \
                '  --disable  root: снять capabilities с этого помощника' \
                'Переносной/user-каталог не подходит для выдачи capabilities. Нужна системная установка.'
            exit 0 ;;
        *) zr_die "Неизвестный аргумент: $1" ;;
    esac
    shift
done

if [[ $action == diagnose ]]; then
    zr_print_icmp
    if caps=$(zr_caps "$BASE/bin/zarnitsa" 2>/dev/null); then
        if [[ -n $caps ]]; then zr_warn 'Qt-программа имеет capabilities — это небезопасно. Переустановите комплект.'; else printf 'Qt-программа: capabilities нет (так и должно быть).\n'; fi
    fi
    printf 'Изменений нет. Fallback: штатный /usr/bin/ping, если разрешён его запуск.\n'
    exit 0
fi
(( EUID == 0 )) || zr_die 'Для изменения capabilities нужны права root. Пользовательский запуск/диагностика sudo не требуют.'

helper=$BASE/bin/zarnitsa-icmp
if zr_is_deb "$BASE"; then
    [[ $BASE == /opt/zarnitsa-deb ]] && zr_marker "$BASE/DEBIAN_PACKAGE" 'ZARNITSA_DEBIAN_PACKAGE=1' || {
        # Значение маркера не исполняется: для ранних пакетов достаточно обычного root-owned файла.
        [[ $BASE == /opt/zarnitsa-deb && -f $BASE/DEBIAN_PACKAGE && ! -L $BASE/DEBIAN_PACKAGE ]] || zr_die 'Неподдерживаемое расположение deb-помощника'
    }
else
    zr_read_version "$BASE" || exit 1
    [[ $BASE == "/opt/zarnitsa/releases/$ZR_RELEASE_ID" ]] || zr_die 'Нельзя выдавать capabilities в переносном или пользовательском каталоге: его можно подменить. Выполните системную установку sudo ./install.sh; затем sudo /opt/zarnitsa/current/configure-icmp.sh --enable.'
    ZR_SCOPE=system
    zr_installation_marker /opt/zarnitsa && zr_marker "$BASE/.zarnitsa-release" 'ZARNITSA_RELEASE=1' || zr_die 'Не найден маркер системной установки tar-комплекта'
fi
zr_secure_tree "$BASE" || exit 1
zr_secure_path "$helper" || exit 1
[[ $(stat -c '%h' -- "$helper") == 1 ]] || zr_die 'У помощника несколько жёстких ссылок; capabilities не менялись'
[[ ! -u $helper && ! -g $helper ]] || zr_die 'Обнаружен setuid/setgid. Переустановите комплект без этих битов; скрипт их не назначает.'

if [[ $action == enable ]]; then
    zr_verify_bundle "$BASE" || exit 1
    zr_static_helper "$helper" || exit 1
    [[ ! -u $BASE/bin/zarnitsa && ! -g $BASE/bin/zarnitsa ]] || zr_die 'У Qt-программы не должно быть setuid/setgid'
    if caps=$(zr_caps "$BASE/bin/zarnitsa" 2>/dev/null); then
        [[ -z $caps ]] || zr_die 'У Qt-программы уже есть capabilities. Отказываюсь продолжать; переустановите проверенный комплект.'
    fi
fi
if ! zr_tool_command setcap; then
    zr_die 'setcap не найден ни в системе, ни в проверенном runtime. Capabilities не назначены, setuid не используется. Fallback — системный /usr/bin/ping с его штатными разрешениями.'
fi
if [[ $action == enable ]]; then
    "${ZR_TOOL_COMMAND[@]}" cap_net_raw+ep "$helper" || zr_die 'Не удалось назначить cap_net_raw (возможно, файловая система/nosuid не поддерживает file capabilities). Используйте штатный /usr/bin/ping.'
    printf 'Разрешение cap_net_raw+ep назначено только %s\n' "$helper"
else
    if caps=$(zr_caps "$helper" 2>/dev/null) && [[ -z $caps ]]; then
        printf 'У помощника уже нет capabilities: %s\n' "$helper"
        exit 0
    fi
    # zr_caps мог заменить массив команды своим getcap, поэтому разрешаем setcap заново.
    zr_tool_command setcap || zr_die 'setcap недоступен'
    "${ZR_TOOL_COMMAND[@]}" -r "$helper" || zr_die 'Не удалось снять capabilities'
    printf 'Capabilities сняты с %s\n' "$helper"
fi
printf 'Qt-бинарник не изменялся. ping_group_range и сетевые правила не менялись.\n'
