#!/bin/bash
# Запуск из любой рабочей папки, в том числе через стабильную ссылку current.
set -euo pipefail
SELF=$(readlink -f -- "${BASH_SOURCE[0]}")
BASE=${SELF%/*}
# shellcheck source=zarnitsa-common.sh
source "$BASE/zarnitsa-common.sh"
ZR_BASE=$BASE

usage() {
    printf '%s\n' 'Зарница — запуск из оффлайн-комплекта' \
        '  zarnitsa [--qt-runtime auto|system|bundled] [параметры] [адрес…]' \
        '  ZARNITSA_QT_RUNTIME=auto|system|bundled zarnitsa [параметры]' \
        '  --headless                 без окна и без X' \
        '  --config ФАЙЛ              свой конфиг (данные zarnitsa-data рядом)' \
        '  --api 127.0.0.1:8790        API только на loopback' \
        '  --no-api                   не включать API в headless' \
        '  --list ФАЙЛ                добавить цели из списка' \
        '  --screenshot ФАЙЛ          разовый PNG; без DISPLAY — offscreen' \
        '  --render-icons КАТАЛОГ     разово нарисовать иконки; без DISPLAY — offscreen' \
        'Примеры целей: 10.0.0.1 host:80 https://сервер/ sip:10.0.0.5' \
        'Полная справка CLI приложения: bin/zarnitsa --headless --help' \
        'Графическую программу запускайте обычным пользователем, без sudo.'
}

mode=${ZARNITSA_QT_RUNTIME:-auto}
headless=0
render=0
options=1
args=()
while (( $# )); do
    if (( options )); then
        case $1 in
            -h|--help) usage; exit 0 ;;
            --qt-runtime)
                (( $# >= 2 )) || zr_die 'После --qt-runtime нужен auto, system или bundled'
                mode=$2; shift 2; continue ;;
            --qt-runtime=*) mode=${1#*=}; shift; continue ;;
            --) options=0 ;;
            --headless) headless=1 ;;
            --screenshot|--screenshot=*|--render-icons|--render-icons=*) render=1 ;;
        esac
    fi
    args+=("$1")
    shift
done

if (( EUID == 0 && ! headless )); then
    zr_die 'Не запускайте графическую Зарницу от root. Откройте её обычным пользователем; sudo нужен только для системной установки/настройки ICMP.'
fi
[[ ! -u $BASE/bin/zarnitsa && ! -g $BASE/bin/zarnitsa ]] || zr_die 'Графический бинарник имеет setuid/setgid. Переустановите проверенный комплект.'
if caps=$(zr_caps "$BASE/bin/zarnitsa" 2>/dev/null); then
    [[ -z $caps ]] || zr_die 'У bin/zarnitsa есть capabilities. Они разрешены только отдельному статическому bin/zarnitsa-icmp, не Qt-программе.'
fi

platform=xcb
if (( headless )); then
    platform=headless
elif (( render )) && [[ -z ${DISPLAY:-} ]]; then
    # Не превращаем обычное открытие окна в невидимый offscreen-процесс.
    export QT_QPA_PLATFORM=offscreen
    platform=offscreen
elif [[ ${QT_QPA_PLATFORM:-} == offscreen || ${QT_QPA_PLATFORM:-} == minimal ]]; then
    platform=$QT_QPA_PLATFORM
fi

if ! zr_select_runtime "$mode" "$platform"; then
    zr_error 'Не найден совместимый Qt 5.15 и его зависимости. Запустите check-runtime.sh или diagnose.sh.'
    [[ -z $ZR_SYSTEM_REPORT ]] || printf '\nСистемный Qt:%s\n' "$ZR_SYSTEM_REPORT" >&2
    [[ -z $ZR_BUNDLED_REPORT ]] || printf '\nQt из комплекта:%s\n' "$ZR_BUNDLED_REPORT" >&2
    exit 1
fi

# Не наследуем loader/plugin-пути от другой Qt-программы. Настройки X/DBus не меняем.
unset LD_PRELOAD LD_AUDIT
export LD_LIBRARY_PATH=$ZR_SELECTED_LIBS
export QT_PLUGIN_PATH=$ZR_SELECTED_PLUGINS
export QT_QPA_PLATFORM_PLUGIN_PATH=$ZR_SELECTED_PLUGINS/platforms
export ZARNITSA_QT_SELECTED=$ZR_SELECTED_RUNTIME
if [[ $ZR_SELECTED_RUNTIME == bundled && -d $BASE/runtime/share/X11/xkb ]]; then
    export XKB_CONFIG_ROOT=$BASE/runtime/share/X11/xkb
fi
# В system-режиме существующий XKB_CONFIG_ROOT не переопределяем.
if [[ $ZR_SELECTED_RUNTIME == bundled && -f $BASE/runtime/etc/fonts/fonts.conf ]]; then
    export FONTCONFIG_FILE=$BASE/runtime/etc/fonts/fonts.conf
elif [[ $ZR_SELECTED_RUNTIME == system && ${FONTCONFIG_FILE:-} == "$BASE/runtime/etc/fonts/fonts.conf" ]]; then
    # Не переносим настройку нашего bundled-runtime в системный Qt.
    unset FONTCONFIG_FILE
fi
# Посторонний пользовательский FONTCONFIG_FILE в system-режиме сохраняется.
# cwd намеренно не меняется: относительные --config/--list/--screenshot относятся к cwd пользователя.
exec "$BASE/bin/zarnitsa" "${args[@]}"
