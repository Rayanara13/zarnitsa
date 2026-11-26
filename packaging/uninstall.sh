#!/bin/bash
# Удаляются только проверенный код и однозначно наши интеграционные файлы.
# Конфиги, история, /var/lib/zarnitsa и учётная запись службы сохраняются.
set -euo pipefail
SELF=$(readlink -f -- "${BASH_SOURCE[0]}")
BASE=${SELF%/*}
source "$BASE/zarnitsa-common.sh"
ZR_BASE=$BASE
scope=user; (( EUID != 0 )) || scope=system
yes=0
while (( $# )); do
    case $1 in
        --user) scope=user ;;
        --system) scope=system ;;
        --yes) yes=1 ;;
        -h|--help)
            printf '%s\n' 'uninstall.sh [--user|--system] [--yes]' \
                'Удаление только tar-установки. До удаления выводятся пути и запрашивается подтверждение.' \
                'Данные/конфиги/история и учётная запись службы не удаляются.'
            exit 0 ;;
        *) zr_die "Неизвестный аргумент: $1" ;;
    esac
    shift
done
if zr_is_deb "$BASE"; then zr_die 'Это .deb-установка. Удаляйте через dpkg -r zarnitsa. /opt/zarnitsa-deb и /usr/bin/zarnitsa tar-uninstall не трогает.'; fi
[[ $scope != system || $EUID == 0 ]] || zr_die 'Для удаления системной tar-установки нужны sudo/root'
[[ $scope != user || $EUID != 0 ]] || zr_die 'Пользовательскую установку удаляйте без sudo, от её владельца'
zr_default_paths "$scope"
if [[ $scope == user && -f $BASE/.zarnitsa-release && $BASE == */zarnitsa-program/releases/* ]]; then
    candidate=${BASE%/releases/*}
    if zr_installation_marker "$candidate"; then
        ZR_PREFIX=$candidate
        ZR_DATA_HOME=${candidate%/zarnitsa-program}
        ZR_DESKTOP=$ZR_DATA_HOME/applications/zarnitsa.desktop
        ZR_ICONS=$ZR_DATA_HOME/icons/hicolor
    fi
fi
[[ ! -L $ZR_PREFIX ]] || zr_die 'Каталог программы не должен быть символической ссылкой'
if [[ $scope == system ]]; then
    [[ $(readlink -m -- "$ZR_PREFIX") == "$ZR_PREFIX" ]] || zr_die 'Системный /opt/zarnitsa не должен перенаправляться ссылками'
fi
ZR_PREFIX=$(readlink -m -- "$ZR_PREFIX")
[[ -d $ZR_PREFIX && ! -L $ZR_PREFIX ]] || zr_die "Установка не найдена: $ZR_PREFIX"
zr_installation_marker "$ZR_PREFIX" || zr_die "Нет нашего маркера и владельца установки: $ZR_PREFIX"
[[ -d $ZR_PREFIX/releases && ! -L $ZR_PREFIX/releases ]] || zr_die 'Нет обычного каталога releases'
if [[ $scope == system ]]; then zr_secure_tree "$ZR_PREFIX" || exit 1; fi
lock=$ZR_PREFIX/.install.lock
mkdir -m 0700 -- "$lock" 2>/dev/null || zr_die "Установка/удаление уже выполняется либо осталась блокировка: $lock"
cleanup() { rmdir -- "$lock" 2>/dev/null || :; }
trap cleanup EXIT

releases=()
targets=()
while IFS= read -r -d '' release; do
    [[ -d $release && ! -L $release ]] || zr_die "Посторонний объект в releases, удаление отменено: $release"
    target=releases/${release##*/}
    zr_check_installed_release "$ZR_PREFIX" "$target" || zr_die "Релиз изменён или содержит посторонние файлы, ничего не удалено: $release. Сохраните свои файлы отдельно и проверьте установку."
    # Даже ошибочно включённые в manifest пользовательские данные не удаляем.
    data_path=$(find "$release" -xdev \( -name zarnitsa.json -o -name secrets.json -o -name zarnitsa-data -o -name 'zarnitsa.log*' \) -print -quit)
    [[ -z $data_path ]] || zr_die "В коде обнаружены возможные данные пользователя, ничего не удалено: $data_path"
    releases+=("$release"); targets+=("$target")
done < <(find "$ZR_PREFIX/releases" -mindepth 1 -maxdepth 1 -print0)
for link in current previous; do
    if [[ -e $ZR_PREFIX/$link || -L $ZR_PREFIX/$link ]]; then
        [[ -L $ZR_PREFIX/$link ]] || zr_die "Не наша ссылка: $ZR_PREFIX/$link"
        target=$(readlink -- "$ZR_PREFIX/$link")
        found=0
        for candidate in "${targets[@]}"; do [[ $candidate != "$target" ]] || found=1; done
        (( found )) || zr_die "Ссылка $link указывает не на проверенный релиз; ничего не удалено"
    fi
done
owned_bin=0; owned_desktop=0; owned_unit=0
if [[ -L $ZR_BIN && $(readlink -- "$ZR_BIN") == "$ZR_PREFIX/current/zarnitsa.sh" ]]; then owned_bin=1; fi
if zr_owned_desktop; then owned_desktop=1; fi
if zr_owned_unit; then owned_unit=1; fi
icons=()
if [[ -d $ZR_ICONS ]]; then
    while IFS= read -r -d '' icon; do
        if zr_owned_icon "$icon"; then icons+=("$icon"); fi
    done < <(find "$ZR_ICONS" -type l -name zarnitsa.png -print0)
fi

printf 'Будут удалены только проверенные каталоги кода:\n'
printf '  %s\n' "${releases[@]}"
printf 'Наши ссылки и файлы интеграции:\n'
(( ! owned_bin )) || printf '  %s\n' "$ZR_BIN"
(( ! owned_desktop )) || printf '  %s\n' "$ZR_DESKTOP"
(( ! owned_unit )) || printf '  %s (наша служба будет остановлена/отключена)\n' "$ZR_UNIT"
for icon in "${icons[@]}"; do printf '  %s\n' "$icon"; done
for link in current previous; do [[ ! -L $ZR_PREFIX/$link ]] || printf '  %s\n' "$ZR_PREFIX/$link"; done
printf 'СОХРАНЯЮТСЯ: %s/zarnitsa/, %s/zarnitsa/, /var/lib/zarnitsa/ и любые внешние --config/журналы.\n' "$ZR_CONFIG_HOME" "$ZR_DATA_HOME"
printf 'Чужие бинарники/ярлыки/службы, /opt/zarnitsa-deb и /usr/bin/zarnitsa не трогаются.\n'
if (( ! yes )); then
    zr_confirm 'Удалить перечисленный код Зарницы?' || {
        rc=$?; printf 'Отменено, ничего не удалено.\n'
        if (( rc == 1 )); then exit 0; else exit 1; fi
    }
fi

if (( owned_unit )); then
    # Не останавливаем одноимённый unit из другого prefix/более приоритетного каталога.
    zr_need systemctl
    ctl=(systemctl); [[ $scope != user ]] || ctl+=(--user)
    if ! fragment=$("${ctl[@]}" show --property=FragmentPath --value zarnitsa.service 2>/dev/null); then
        zr_die 'Нет связи с systemd: нельзя безопасно проверить и остановить свою службу. Код не удалён; повторите в её пользовательской/системной сессии.'
    fi
    if [[ -n $fragment && $(readlink -m -- "$fragment") == "$(readlink -m -- "$ZR_UNIT")" ]]; then
        "${ctl[@]}" stop zarnitsa.service || zr_die 'Не удалось остановить свою службу; код не удалён'
        "${ctl[@]}" disable zarnitsa.service || zr_die 'Не удалось отключить свою службу; код не удалён'
    elif [[ -n $fragment ]]; then
        zr_warn "Одноимённая служба загружена из $fragment: не останавливаю и не отключаю её"
    fi
    zr_owned_unit || zr_die 'Unit изменился во время удаления; операция остановлена'
    rm -- "$ZR_UNIT"
    "${ctl[@]}" daemon-reload || zr_warn 'Выполните daemon-reload в соответствующем systemd вручную'
fi
if (( owned_bin )); then
    [[ -L $ZR_BIN && $(readlink -- "$ZR_BIN") == "$ZR_PREFIX/current/zarnitsa.sh" ]] || zr_die 'Ссылка запуска изменилась во время удаления'
    rm -- "$ZR_BIN"
fi
if (( owned_desktop )); then zr_owned_desktop || zr_die 'Ярлык изменился во время удаления'; rm -- "$ZR_DESKTOP"; fi
for icon in "${icons[@]}"; do
    zr_owned_icon "$icon" || zr_die "Иконка изменилась во время удаления: $icon"
    rm -- "$icon"
done
for link in current previous; do [[ ! -L $ZR_PREFIX/$link ]] || rm -- "$ZR_PREFIX/$link"; done
for release in "${releases[@]}"; do
    # Повторно проверяем перед удалением, не переходим на вложенную файловую систему.
    zr_verify_bundle "$release" || zr_die "Релиз изменился во время удаления, сохранён: $release"
    rm -rf --one-file-system -- "$release"
done
rmdir -- "$ZR_PREFIX/releases" 2>/dev/null || :
remaining=$(find "$ZR_PREFIX" -mindepth 1 -maxdepth 1 ! -name .zarnitsa-installation ! -name .install.lock -print -quit)
if [[ -z $remaining ]]; then
    rm -- "$ZR_PREFIX/.zarnitsa-installation"
    rmdir -- "$lock"
    trap - EXIT
    rmdir -- "$ZR_PREFIX"
else
    zr_warn "Оставлены не относящиеся к проверенному коду файлы: $ZR_PREFIX"
fi
if command -v update-desktop-database >/dev/null 2>&1; then update-desktop-database "${ZR_DESKTOP%/*}" >/dev/null 2>&1 || :; fi
printf 'Проверенный код удалён. Конфиг, история, журналы и учётная запись службы сохранены.\n'
