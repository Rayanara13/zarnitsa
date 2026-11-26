#!/bin/bash
# Оффлайн-установка tar-комплекта. Сеть, apt и пользовательские данные не используются.
set -euo pipefail
umask 022
SELF=$(readlink -f -- "${BASH_SOURCE[0]}")
BASE=${SELF%/*}
source "$BASE/zarnitsa-common.sh"
ZR_BASE=$BASE

usage() {
    printf '%s\n' \
        'Зарница — установка оффлайн-комплекта' \
        '  ./install.sh [--user|--system] [--service]' \
        '  ./install.sh [--user|--system] --rollback' \
        '  ./install.sh [--user|--system] --print-service' \
        'Без root: XDG_DATA_HOME/zarnitsa-program; с root: /opt/zarnitsa.' \
        '--service только записывает unit; не включает и не запускает службу.' \
        '--print-service только печатает unit, ничего не устанавливает.' \
        '--rollback меняет current/previous между проверенными релизами; данные сохраняются.'
}

scope=user; (( EUID != 0 )) || scope=system
service=0; rollback=0; print_service=0
while (( $# )); do
    case $1 in
        --user) scope=user ;;
        --system) scope=system ;;
        --service) service=1 ;;
        --rollback) rollback=1 ;;
        --print-service) print_service=1 ;;
        -h|--help) usage; exit 0 ;;
        *) zr_die "Неизвестный аргумент: $1 (см. --help)" ;;
    esac
    shift
done
[[ $scope != system || $EUID == 0 ]] || zr_die 'Системную установку выполняют через sudo/root; без sudo используйте --user'
[[ $scope != user || $EUID != 0 ]] || zr_die 'Пользовательскую установку выполняйте без sudo, от владельца HOME'
(( ! rollback || (! service && ! print_service) )) || zr_die '--rollback не совмещается с --service/--print-service'
if zr_is_deb "$BASE"; then zr_die 'Это комплект .deb: устанавливайте через dpkg -i, удаляйте через dpkg -r zarnitsa. Tar-install не управляет /opt/zarnitsa-deb.'; fi
zr_default_paths "$scope"

# Из установленного user-релиза можно удалить/откатить именно его XDG-prefix.
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
zr_plain_path "$ZR_PREFIX" || zr_die 'Недопустимый путь установки'

print_unit() {
    printf '# ZARNITSA_MANAGED_UNIT=1\n# ZARNITSA_INSTALL_ROOT=%s\n' "$ZR_PREFIX"
    printf '[Unit]\nDescription=Зарница — мониторинг сети без окна\nAfter=network.target\n\n[Service]\nType=simple\n'
    if [[ $scope == system ]]; then
        printf '%s\n' 'User=zarnitsa' 'Group=zarnitsa' 'WorkingDirectory=/var/lib/zarnitsa'
        printf 'ExecStart=%s --headless --config /var/lib/zarnitsa/zarnitsa.json --api 127.0.0.1:8790\n' "$(zr_systemd_quote "$ZR_PREFIX/current/zarnitsa.sh")"
        # NNP=no необходим для filecap helper на exec. BoundingSet ограничивает потолок,
        # но не выдаёт capabilities Qt-процессу; AmbientCapabilities намеренно отсутствует.
        printf '%s\n' '# NoNewPrivileges=no: filecap получает только статический helper, не Qt-процесс.' \
            '# AmbientCapabilities не задаётся; BoundingSet — ограничение, а не выдача прав.' \
            'NoNewPrivileges=no' 'CapabilityBoundingSet=CAP_NET_RAW' \
            'PrivateTmp=yes' 'ProtectSystem=strict' 'ProtectHome=yes' 'ReadWritePaths=/var/lib/zarnitsa' \
            'RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6'
    else
        printf 'Environment=%s %s\n' "$(zr_systemd_quote "XDG_DATA_HOME=$ZR_DATA_HOME")" "$(zr_systemd_quote "XDG_CONFIG_HOME=$ZR_CONFIG_HOME")"
        printf 'ExecStart=%s --headless --api 127.0.0.1:8790\n' "$(zr_systemd_quote "$ZR_PREFIX/current/zarnitsa.sh")"
        # User-manager не может сам получить CAP_NET_RAW. Штатному ping не мешаем использовать filecap.
        printf '%s\n' 'NoNewPrivileges=no'
    fi
    # В журнал направляются статусы; флаг --print-api-token служба никогда не передаёт.
    printf '%s\n' 'UMask=0077' 'StandardOutput=journal' 'StandardError=journal' 'Restart=on-failure' 'RestartSec=5' '' '[Install]'
    if [[ $scope == system ]]; then printf 'WantedBy=multi-user.target\n'; else printf 'WantedBy=default.target\n'; fi
}

if (( print_service )); then print_unit; exit 0; fi
for cmd in sha256sum find stat readlink cp chmod install mv ln mktemp; do zr_need "$cmd"; done

system_destination() {
    local path=$1 parent=$1
    [[ $(readlink -m -- "$path") == "$path" ]] || zr_die "Системный путь не должен перенаправляться ссылкой: $path"
    while [[ ! -e $parent ]]; do parent=${parent%/*}; [[ -n $parent ]] || parent=/; done
    zr_secure_path "$parent" || exit 1
}

if [[ $scope == system ]]; then
    for dest in "$ZR_PREFIX" "${ZR_BIN%/*}" "${ZR_DESKTOP%/*}" "$ZR_ICONS"; do system_destination "$dest"; done
    (( ! service )) || system_destination "${ZR_UNIT%/*}"
fi
[[ ! -L $ZR_PREFIX && (! -e $ZR_PREFIX || -d $ZR_PREFIX) ]] || zr_die "Путь установки занят не каталогом: $ZR_PREFIX"
if [[ -d $ZR_PREFIX ]]; then
    if ! zr_installation_marker "$ZR_PREFIX"; then
        [[ -z $(find "$ZR_PREFIX" -mindepth 1 -maxdepth 1 -print -quit) ]] || zr_die "Нельзя использовать непустой чужой каталог: $ZR_PREFIX"
    fi
fi
if [[ -e $ZR_BIN || -L $ZR_BIN ]]; then
    [[ -L $ZR_BIN && $(readlink -- "$ZR_BIN") == "$ZR_PREFIX/current/zarnitsa.sh" ]] || zr_die "Не перезаписываю чужой бинарник/ссылку: $ZR_BIN"
fi
if [[ -e $ZR_DESKTOP || -L $ZR_DESKTOP ]]; then
    zr_owned_desktop || zr_die "Не перезаписываю чужой ярлык: $ZR_DESKTOP"
fi
if (( service )) && [[ -e $ZR_UNIT || -L $ZR_UNIT ]]; then
    zr_owned_unit || zr_die "Не перезаписываю чужую службу: $ZR_UNIT"
fi

if (( rollback )); then
    zr_installation_marker "$ZR_PREFIX" || zr_die 'Управляемая установка не найдена; откатывать нечего'
else
    printf 'Проверяю весь SHA256SUMS до копирования…\n'
    zr_verify_bundle "$BASE" || exit 1
    new_id=$ZR_RELEASE_ID
    source_manifest=$(zr_manifest_digest "$BASE")
    zr_static_helper "$BASE/bin/zarnitsa-icmp" || exit 1
fi

[[ ! -L $ZR_PREFIX/releases && (! -e $ZR_PREFIX/releases || -d $ZR_PREFIX/releases) ]] || zr_die 'releases должен быть обычным каталогом'
install -d -m 0755 -- "$ZR_PREFIX" "$ZR_PREFIX/releases"
if ! zr_installation_marker "$ZR_PREFIX"; then
    printf 'ZARNITSA_INSTALLATION=1\nSCOPE=%s\nOWNER_UID=%s\n' "$scope" "$EUID" > "$ZR_PREFIX/.zarnitsa-installation"
    chmod 0644 -- "$ZR_PREFIX/.zarnitsa-installation"
fi
[[ ! -L $ZR_PREFIX/releases ]] || zr_die 'releases не должен быть символической ссылкой'
if [[ $scope == system ]]; then zr_secure_path "$ZR_PREFIX/releases" || exit 1; fi
lock=$ZR_PREFIX/.install.lock
mkdir -m 0700 -- "$lock" 2>/dev/null || zr_die "Установка/удаление уже выполняется, либо осталась блокировка: $lock. Проверьте процесс перед ручным удалением пустой блокировки."
stage=; switch_dir=
cleanup() {
    local rc=$?
    [[ -z $stage ]] || rm -rf -- "$stage"
    [[ -z $switch_dir ]] || rm -rf -- "$switch_dir"
    rmdir -- "$lock" 2>/dev/null || :
    return "$rc"
}
trap cleanup EXIT

old_target=
if [[ -e $ZR_PREFIX/current || -L $ZR_PREFIX/current ]]; then
    [[ -L $ZR_PREFIX/current ]] || zr_die 'current занят не нашей символической ссылкой'
    old_target=$(readlink -- "$ZR_PREFIX/current")
    zr_check_installed_release "$ZR_PREFIX" "$old_target" || zr_die 'Текущий релиз не прошёл проверку; переключение отменено'
fi
if [[ -e $ZR_PREFIX/previous || -L $ZR_PREFIX/previous ]]; then
    [[ -L $ZR_PREFIX/previous ]] || zr_die 'previous занят не символической ссылкой'
    previous_target=$(readlink -- "$ZR_PREFIX/previous")
    zr_check_installed_release "$ZR_PREFIX" "$previous_target" || zr_die 'Предыдущий релиз не прошёл проверку'
else
    previous_target=
fi

if (( rollback )); then
    [[ -n $old_target && -n $previous_target && $old_target != "$previous_target" ]] || zr_die 'Нет отличающейся предыдущей проверенной версии'
    new_target=$previous_target
    dest=$ZR_PREFIX/$new_target
    printf 'Откат: %s → %s\n' "$old_target" "$new_target"
else
    dest=$ZR_PREFIX/releases/$new_id
    new_target=releases/$new_id
    if [[ -e $dest || -L $dest ]]; then
        zr_check_installed_release "$ZR_PREFIX" "$new_target" || zr_die 'Существующий релиз с таким ID не прошёл проверку'
        [[ $(zr_manifest_digest "$dest") == "$source_manifest" ]] || zr_die 'Такой VERSION-REVISION уже установлен с другим содержимым; нужна новая REVISION'
        printf 'Проверенный релиз уже существует: %s\n' "$dest"
    else
        stage=$(mktemp -d "$ZR_PREFIX/releases/.new-$new_id.XXXXXX")
        cp -R --no-preserve=ownership,mode,xattr -- "$BASE/." "$stage/"
        [[ $(zr_manifest_digest "$stage") == "$source_manifest" ]] || zr_die 'Исходный SHA256SUMS изменился во время копирования'
        zr_verify_bundle "$stage" || exit 1
        # Никаких setuid/setgid/xattr из исходного каталога. Все права задаём заново.
        find "$stage" -type d -exec chmod 0755 -- {} +
        find "$stage" -type f -exec chmod 0644 -- {} +
        chmod 0755 -- "$stage/bin/zarnitsa" "$stage/bin/zarnitsa-icmp" "$stage/"*.sh
        if [[ -d $stage/runtime/bin ]]; then find "$stage/runtime/bin" -type f -exec chmod 0755 -- {} +; fi
        printf 'ZARNITSA_RELEASE=1\nID=%s\nMANIFEST_SHA256=%s\n' "$new_id" "$source_manifest" > "$stage/.zarnitsa-release"
        chmod 0644 -- "$stage/.zarnitsa-release"
        if [[ $scope == system ]]; then
            chown -hR root:root -- "$stage"
            zr_secure_tree "$stage" || exit 1
        fi
        zr_verify_bundle "$stage" || exit 1
        mv -T -- "$stage" "$dest"
        stage=
    fi
fi

# Иконки устанавливаем своими ссылками на current; существующие чужие файлы не трогаем.
icon_names=()
while IFS= read -r -d '' icon; do
    name=${icon#"$dest/icons/"}
    [[ $name =~ ^[0-9]+x[0-9]+/apps/zarnitsa\.png$ ]] || continue
    icon_names+=("$name")
    if [[ $scope == system ]]; then system_destination "$ZR_ICONS/${name%/*}"; fi
    if [[ -e $ZR_ICONS/$name || -L $ZR_ICONS/$name ]]; then
        zr_owned_icon "$ZR_ICONS/$name" || zr_die "Не перезаписываю чужую иконку: $ZR_ICONS/$name"
    fi
done < <(find "$dest/icons" -type f -name zarnitsa.png -print0)
(( ${#icon_names[@]} > 0 )) || zr_die 'В комплекте нет иконок <размер>x<размер>/apps/zarnitsa.png'

if [[ $scope == system ]]; then
    zr_secure_tree "$dest" || exit 1
    # Права/владельцы уже защищены. Capabilities получает только статический помощник.
    if ! "$dest/configure-icmp.sh" --enable; then
        zr_warn 'cap_net_raw не выдан. Установка продолжится: доступен fallback на системный /usr/bin/ping, если его разрешения позволяют. setuid не назначается.'
    fi
fi

switch_dir=$(mktemp -d "$ZR_PREFIX/.switch.XXXXXX")
install -d -m 0755 -- "${ZR_BIN%/*}" "${ZR_DESKTOP%/*}"
ln -s -- "$ZR_PREFIX/current/zarnitsa.sh" "$switch_dir/launcher"
mv -Tf -- "$switch_dir/launcher" "$ZR_BIN"
template=$(<"$dest/zarnitsa.desktop.in")
exec_value=$(zr_desktop_exec_quote "$ZR_BIN")
template=${template//@ZARNITSA_EXEC@/$exec_value}
template=${template//@ZARNITSA_TRY_EXEC@/$ZR_BIN}
template=${template//@ZARNITSA_ROOT@/$ZR_PREFIX}
printf '%s\n' "$template" > "$switch_dir/zarnitsa.desktop"
chmod 0644 -- "$switch_dir/zarnitsa.desktop"
mv -Tf -- "$switch_dir/zarnitsa.desktop" "$ZR_DESKTOP"
for name in "${icon_names[@]}"; do
    install -d -m 0755 -- "${ZR_ICONS}/${name%/*}"
    ln -s -- "$ZR_PREFIX/current/icons/$name" "$switch_dir/icon"
    mv -Tf -- "$switch_dir/icon" "$ZR_ICONS/$name"
done

if (( service )); then
    if [[ $scope == system ]]; then
        zr_need getent
        if account=$(getent passwd zarnitsa); then
            IFS=: read -r account_name _ account_uid account_gid _ account_home account_shell <<< "$account"
            [[ $account_name == zarnitsa && $account_uid != 0 && $account_home == /var/lib/zarnitsa && $account_shell =~ /(nologin|false)$ ]] || zr_die 'Имя zarnitsa уже занято посторонней учётной записью'
            group=$(getent group zarnitsa) || zr_die 'Для существующей служебной записи нет группы zarnitsa'
            IFS=: read -r _ _ group_gid _ <<< "$group"
            [[ $group_gid == "$account_gid" ]] || zr_die 'Основная группа существующей записи zarnitsa отличается от служебной'
        else
            zr_need useradd
            useradd --system --user-group --home-dir /var/lib/zarnitsa --no-create-home --shell /usr/sbin/nologin zarnitsa
        fi
        [[ ! -L /var/lib/zarnitsa ]] || zr_die '/var/lib/zarnitsa не должен быть ссылкой'
        if [[ -e /var/lib/zarnitsa ]]; then
            [[ -d /var/lib/zarnitsa && $(stat -c '%u' /var/lib/zarnitsa) == "$(id -u zarnitsa)" ]] || zr_die 'Существующий /var/lib/zarnitsa не принадлежит служебному пользователю; не меняю его данные'
        else
            install -d -m 0700 -o zarnitsa -g zarnitsa /var/lib/zarnitsa
        fi
    fi
    install -d -m 0755 -- "${ZR_UNIT%/*}"
    print_unit > "$switch_dir/zarnitsa.service"
    chmod 0644 -- "$switch_dir/zarnitsa.service"
    mv -Tf -- "$switch_dir/zarnitsa.service" "$ZR_UNIT"
fi

# Переключение выполняется после проверки и интеграции. Старые releases не удаляются.
if [[ -n $old_target && $old_target != "$new_target" ]]; then
    ln -s -- "$old_target" "$switch_dir/previous"
    mv -Tf -- "$switch_dir/previous" "$ZR_PREFIX/previous"
fi
ln -s -- "$new_target" "$switch_dir/current"
mv -Tf -- "$switch_dir/current" "$ZR_PREFIX/current"

if command -v update-desktop-database >/dev/null 2>&1; then update-desktop-database "${ZR_DESKTOP%/*}" >/dev/null 2>&1 || :; fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then gtk-update-icon-cache -q -t "$ZR_ICONS" >/dev/null 2>&1 || :; fi
printf '\nГотово. Код: %s\nЗапуск: %s\nЯрлык: %s\n' "$dest" "$ZR_BIN" "$ZR_DESKTOP"
printf 'Конфиг %s/zarnitsa/ и данные %s/zarnitsa/ не менялись.\n' "$ZR_CONFIG_HOME" "$ZR_DATA_HOME"
if (( service )); then
    printf 'Unit записан: %s\nСлужба НЕ включена и НЕ запущена. API при запуске: только 127.0.0.1:8790.\n' "$ZR_UNIT"
    if [[ $scope == system ]]; then
        printf 'Для явного запуска: sudo systemctl daemon-reload && sudo systemctl enable --now zarnitsa.service\n'
        printf 'Конфиг службы: /var/lib/zarnitsa/zarnitsa.json; данные: /var/lib/zarnitsa/zarnitsa-data\n'
    else
        printf 'Для явного запуска: systemctl --user daemon-reload && systemctl --user enable --now zarnitsa.service\n'
    fi
fi
