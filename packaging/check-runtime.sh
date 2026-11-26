#!/bin/bash
# Только чтение: выбор Qt проверяется ldd -r, приложение не запускается.
set -euo pipefail
SELF=$(readlink -f -- "${BASH_SOURCE[0]}")
BASE=${SELF%/*}
source "$BASE/zarnitsa-common.sh"
ZR_BASE=$BASE
mode=${ZARNITSA_QT_RUNTIME:-auto}
platform=xcb
while (( $# )); do
    case $1 in
        --qt-runtime)
            (( $# >= 2 )) || zr_die 'После --qt-runtime нужен режим'
            mode=$2; shift ;;
        --qt-runtime=*) mode=${1#*=} ;;
        --headless) platform=headless ;;
        --offscreen) platform=offscreen ;;
        -h|--help)
            printf '%s\n' 'check-runtime.sh [--qt-runtime auto|system|bundled] [--headless|--offscreen]' \
                'Проверяет ldd -r бинарника и подходящего QPA-плагина, требует Qt 5.15.x.' \
                'Без флагов проверяется xcb; для QCoreApplication (--headless) X/плагины не нужны.' \
                'Ничего не устанавливает, приложение и сетевые проверки не запускает.'
            exit 0 ;;
        *) zr_die "Неизвестный аргумент: $1" ;;
    esac
    shift
done
case $mode in auto|system|bundled) ;; *) zr_die 'Режим Qt: только auto, system, bundled' ;; esac
printf 'Корень программы: %s\nБинарник: %s/bin/zarnitsa\n' "$BASE" "$BASE"
printf 'Режим выбора Qt: %s; платформа: %s\n' "$mode" "$platform"
printf 'Системные glibc, ядро и базовые библиотеки X11/xcb не входят в обещание «без зависимостей».\n'
status=0
zr_select_runtime "$mode" "$platform" || status=1
[[ -z $ZR_SYSTEM_REPORT ]] || printf '\nПроверка системного Qt:%s\n' "$ZR_SYSTEM_REPORT"
[[ -z $ZR_BUNDLED_REPORT ]] || printf '\nПроверка Qt из комплекта:%s\n' "$ZR_BUNDLED_REPORT"
if (( status == 0 )); then
    printf '\nСовместимый режим: %s\nLD_LIBRARY_PATH: %s\nПлагины: %s\n' "$ZR_SELECTED_RUNTIME" "$ZR_SELECTED_LIBS" "$ZR_SELECTED_PLUGINS"
else
    printf '\nСовместимый runtime не найден. Исправьте отсутствующие базовые зависимости или используйте комплект для подходящей Astra/glibc/архитектуры.\n'
fi
if caps=$(zr_caps "$BASE/bin/zarnitsa" 2>/dev/null) && [[ -n $caps ]]; then
    zr_warn 'У Qt-программы есть capabilities: запрещено, нужен чистый бинарник'; status=1
fi
if [[ -u $BASE/bin/zarnitsa || -g $BASE/bin/zarnitsa ]]; then zr_warn 'У Qt-программы есть setuid/setgid: запрещено'; status=1; fi
exit "$status"
