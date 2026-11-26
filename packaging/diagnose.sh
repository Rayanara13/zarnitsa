#!/bin/bash
# Отчёт без чтения конфига/secrets и без сетевых запросов.
set -euo pipefail
SELF=$(readlink -f -- "${BASH_SOURCE[0]}")
BASE=${SELF%/*}
source "$BASE/zarnitsa-common.sh"
ZR_BASE=$BASE
output=
mode=${ZARNITSA_QT_RUNTIME:-auto}
platform=()
while (( $# )); do
    case $1 in
        -o|--output)
            (( $# >= 2 )) || zr_die 'После --output нужен путь нового файла'
            [[ -z $output ]] || zr_die 'Путь отчёта задан дважды'
            output=$2; shift ;;
        --qt-runtime)
            (( $# >= 2 )) || zr_die 'После --qt-runtime нужен режим'
            mode=$2; shift ;;
        --qt-runtime=*) mode=${1#*=} ;;
        --headless|--offscreen) platform=("$1") ;;
        -h|--help)
            printf '%s\n' 'diagnose.sh [--output НОВЫЙ_ФАЙЛ] [--qt-runtime auto|system|bundled] [--headless|--offscreen]' \
                'Без --output выводит отчёт в терминал; существующий файл не перезаписывает.' \
                'Только чтение ОС, путей, зависимостей, capabilities и ping_group_range.' \
                'Конфиги, токены, пароли, окружение сессии и журналы не печатает; сеть не опрашивает.'
            exit 0 ;;
        *) zr_die "Неизвестный аргумент: $1" ;;
    esac
    shift
done
case $mode in auto|system|bundled) ;; *) zr_die 'Режим Qt: только auto, system, bundled' ;; esac

report() {
    local line key value
    printf 'Зарница — диагностика оффлайн-комплекта\n'
    printf 'Время UTC: '; date -u '+%Y-%m-%dT%H:%M:%SZ'
    printf 'ОС:\n'
    if [[ -r /etc/os-release ]]; then
        while IFS= read -r line; do
            key=${line%%=*}
            case $key in PRETTY_NAME|ID|VERSION_ID)
                value=${line#*=}; value=${value#\"}; value=${value%\"}
                printf '  %s=%s\n' "$key" "$value" ;;
            esac
        done < /etc/os-release
    fi
    printf 'Архитектура: '; uname -m
    printf 'Ядро: '; uname -r
    printf 'glibc: '; getconf GNU_LIBC_VERSION 2>/dev/null || printf 'не определена\n'
    printf 'Корень программы: %s\n' "$BASE"
    if [[ -f $BASE/VERSION.txt ]] && zr_read_version "$BASE"; then printf 'Версия: %s; ревизия: %s\n' "$ZR_VERSION" "$ZR_REVISION"; fi
    printf '\nПути (содержимое конфигов/журналов не читается):\n'
    printf '  Конфиг по умолчанию: %s/zarnitsa/zarnitsa.json\n' "${XDG_CONFIG_HOME:-$HOME/.config}"
    printf '  Данные по умолчанию: %s/zarnitsa/\n' "${XDG_DATA_HOME:-$HOME/.local/share}"
    printf '  User-код: %s/zarnitsa-program/current\n' "${XDG_DATA_HOME:-$HOME/.local/share}"
    printf '  Системный tar-код: /opt/zarnitsa/current; deb-код: /opt/zarnitsa-deb\n'
    printf '  Служба: /var/lib/zarnitsa/zarnitsa.json; данные рядом: /var/lib/zarnitsa/zarnitsa-data/\n'
    printf '  Переносной --config ПУТЬ: данные в каталоге zarnitsa-data рядом с ПУТЬ.\n'
    printf 'DISPLAY: '; if [[ -n ${DISPLAY:-} ]]; then printf 'задан (значение скрыто)\n'; else printf 'не задан\n'; fi
    printf 'Переменные XAUTHORITY/DBUS_SESSION_BUS_ADDRESS/XDG_RUNTIME_DIR не менялись и не выводятся.\n'
    if [[ -f $BASE/SHA256SUMS ]]; then
        printf '\nКонтроль целостности всего комплекта:\n'
        if zr_verify_bundle "$BASE"; then printf 'SHA256SUMS: совпадают, непокрытых обычных файлов нет.\n'; else printf 'SHA256SUMS: проверка НЕ пройдена.\n'; fi
    fi
    printf '\nQt и плагины:\n'
    "$BASE/check-runtime.sh" --qt-runtime "$mode" "${platform[@]}" || printf 'Проверка runtime завершилась с ошибкой (см. выше).\n'
    printf '\nICMP (без изменений):\n'
    zr_print_icmp
    printf '\nПример запуска без X и без внешнего API-порта:\n  '
    printf '%q --headless --api 127.0.0.1:8790\n' "$BASE/zarnitsa.sh"
    printf 'Для работы вообще без API: добавьте --no-api.\n'
    printf 'Изменений sysctl, capabilities, firewall, служб и конфигов не производилось.\n'
}

if [[ -z $output ]]; then
    report
else
    [[ ! -e $output && ! -L $output ]] || zr_die 'Файл отчёта уже существует: выберите новый путь (перезапись запрещена)'
    umask 077
    ( set -o noclobber; report > "$output" 2>&1 ) || zr_die 'Не удалось создать отчёт; проверьте путь/права'
    printf 'Отчёт сохранён: %s\n' "$output"
fi
