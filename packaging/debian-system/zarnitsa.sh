#!/bin/bash
# Нативный пакет: только системный Qt, без переносного runtime и смены cwd.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin

# Не наследуем loader/plugin/import-пути другого Qt-комплекта.
# Настройки Fly/HiDPI/input/platform, DISPLAY/XAUTHORITY, DBus, XDG и локаль сохраняем.
for name in "${!LD_@}"; do
    [[ -z $name ]] || unset "$name"
done
unset QT_PLUGIN_PATH QT_QPA_PLATFORM_PLUGIN_PATH QML_IMPORT_PATH QML2_IMPORT_PATH
unset QTDIR QTLIB QMAKEPATH QMAKEFEATURES ZARNITSA_QT_RUNTIME ZARNITSA_QT_SELECTED

zr_die() { printf 'Ошибка: %s\n' "$*" >&2; exit 1; }

usage() {
    printf '%s\n' 'Зарница — запуск с системным Qt' \
        '  zarnitsa [параметры] [адрес…]' \
        '  --headless                 без окна и без X' \
        '  --config ФАЙЛ              свой конфиг (данные zarnitsa-data рядом)' \
        '  --api 127.0.0.1:8790        адрес API; вне loopback нужен TLS' \
        '  --no-api                   не включать API в headless' \
        '  --list ФАЙЛ                добавить цели из списка' \
        '  --screenshot ФАЙЛ          PNG; без DISPLAY по умолчанию offscreen' \
        '  --render-icons КАТАЛОГ     значки; без DISPLAY по умолчанию offscreen' \
        'Примеры целей: 10.0.0.1 host:80 https://сервер/ sip:10.0.0.5' \
        'Полная справка CLI: /usr/lib/zarnitsa/zarnitsa --headless --help' \
        'Пакет использует только Qt операционной системы; --qt-runtime не нужен.' \
        'Графическую программу запускайте обычным пользователем, без sudo.'
}

headless=0
render=0
options=1
for arg in "$@"; do
    if (( options )); then
        case $arg in
            -h|--help) usage; exit 0 ;;
            --) options=0 ;;
            --headless) headless=1 ;;
            --screenshot|--screenshot=*|--render-icons|--render-icons=*) render=1 ;;
            --qt-runtime|--qt-runtime=*) zr_die 'Нативный пакет использует только системный Qt; уберите --qt-runtime.' ;;
        esac
    fi
done

if (( EUID == 0 && ! headless )); then
    zr_die 'Не запускайте графическую Зарницу от root. Откройте её обычным пользователем; root нужен только для установки пакета.'
fi
app=/usr/lib/zarnitsa/zarnitsa
[[ -f $app && ! -L $app && -x $app ]] || zr_die "Нет обычного исполняемого файла $app. Переустановите пакет."
[[ ! -u $app && ! -g $app ]] || zr_die 'У Qt-программы обнаружен setuid/setgid. Переустановите проверенный пакет.'

# Системный инструмент выбирается только по фиксированным путям, не из workspace.
getcap=
for dir in /usr/sbin /usr/bin /sbin /bin; do
    if [[ -f $dir/getcap && -x $dir/getcap ]]; then getcap=$dir/getcap; break; fi
done
[[ -n $getcap ]] || zr_die 'Не найден системный getcap: отсутствие capabilities Qt не проверено. Нужен libcap2-bin.'
caps=$("$getcap" "$app" 2>&1) || zr_die "Не удалось проверить capabilities Qt-программы: $caps"
[[ -z $caps ]] || zr_die 'У Qt-программы есть capabilities. Они разрешены только отдельному ICMP-помощнику; переустановите пакет.'

# Обычное открытие окна не превращаем в невидимый offscreen-процесс.
if (( ! headless && render )) && [[ -z ${DISPLAY:-} && -z ${QT_QPA_PLATFORM:-} ]]; then
    export QT_QPA_PLATFORM=offscreen
fi
# Относительные --config/--list/--screenshot относятся к исходному cwd пользователя.
exec /usr/lib/zarnitsa/zarnitsa "$@"
