#!/bin/bash
# Локальный self-signed ECDSA P-256. Никаких сетевых действий или изменения конфига Зарницы.
set -euo pipefail
umask 077
SELF=$(readlink -f -- "${BASH_SOURCE[0]}")
BASE=${SELF%/*}
source "$BASE/zarnitsa-common.sh"
ZR_BASE=$BASE

dir=; days=365; yes=0
ips=(); names=()
while (( $# )); do
    case $1 in
        --dir)
            (( $# >= 2 )) || zr_die 'После --dir нужен каталог'
            [[ -z $dir ]] || zr_die '--dir задан дважды'
            dir=$2; shift ;;
        --ip)
            (( $# >= 2 )) || zr_die 'После --ip нужен IPv4 или IPv6'
            ips+=("$2"); shift ;;
        --dns)
            (( $# >= 2 )) || zr_die 'После --dns нужно DNS-имя (IDN — в punycode)'
            names+=("$2"); shift ;;
        --days)
            (( $# >= 2 )) || zr_die 'После --days нужно число дней'
            days=$2; shift ;;
        --yes) yes=1 ;;
        -h|--help)
            printf '%s\n' 'make-cert.sh --dir КАТАЛОГ [--ip АДРЕС]… [--dns ИМЯ]… [--days 365] [--yes]' \
                'Нужен хотя бы один SAN: --ip или --dns. Пример:' \
                '  ./make-cert.sh --dir "$HOME/.config/zarnitsa/tls" --ip 127.0.0.1 --dns localhost' \
                'Создаёт только zarnitsa.key (0600) и zarnitsa.crt в указанном каталоге.' \
                'Существующие файлы заменяются только с подтверждением; --yes — явное согласие.' \
                'Предпочитает системный openssl; иначе проверенный runtime/bin/openssl. Сеть не нужна.'
            exit 0 ;;
        *) zr_die "Неизвестный аргумент: $1" ;;
    esac
    shift
done
[[ -n $dir ]] || zr_die 'Укажите --dir; каталога по умолчанию намеренно нет'
(( ${#ips[@]} + ${#names[@]} > 0 )) || zr_die 'Укажите SAN через --ip или --dns; внешние адреса автоматически не добавляются'
[[ $days =~ ^[0-9]{1,4}$ ]] && (( 10#$days >= 1 && 10#$days <= 3650 )) || zr_die '--days: допустимо 1–3650'
days=$((10#$days))

for ip in "${ips[@]}"; do
    if [[ $ip == *:* ]]; then
        [[ $ip =~ ^[0-9A-Fa-f:.]+$ && ${#ip} -le 45 ]] || zr_die 'Некорректный IPv6 (зоны интерфейсов в SAN не допускаются)'
        # Полную проверку IPv6 дополнительно выполняет openssl при разборе subjectAltName.
    else
        [[ $ip =~ ^[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}$ ]] || zr_die 'Некорректный IPv4'
        IFS=. read -r -a octets <<< "$ip"
        for octet in "${octets[@]}"; do (( 10#$octet <= 255 )) || zr_die 'Октет IPv4 превышает 255'; done
    fi
done
for name in "${names[@]}"; do
    [[ ${#name} -le 253 && $name != .* && $name != *. && $name != *..* && $name =~ ^[0-9A-Za-z.-]+$ ]] || zr_die 'Некорректное DNS-имя; используйте ASCII/punycode без управляющих символов'
    IFS=. read -r -a labels <<< "$name"
    for label in "${labels[@]}"; do
        [[ ${#label} -le 63 && $label =~ ^[0-9A-Za-z]([0-9A-Za-z-]*[0-9A-Za-z])?$ ]] || zr_die 'Некорректная DNS-метка'
    done
done
zr_tool_command openssl || zr_die 'Не найден системный openssl или проверенный runtime/bin/openssl. Ничего не создано; apt/сеть скрипт не использует.'
openssl_cmd=("${ZR_TOOL_COMMAND[@]}")

dir=$(readlink -m -- "$dir")
zr_plain_path "$dir" || zr_die 'Недопустимый путь каталога сертификата'
[[ ! -L $dir && (! -e $dir || -d $dir) ]] || zr_die 'Каталог сертификата занят не обычным каталогом'
mkdir -p -- "$dir"
[[ $(stat -c '%u' -- "$dir") == "$EUID" ]] || zr_die 'Каталог сертификата должен принадлежать запускающему пользователю'
mode=$(stat -c '%a' -- "$dir")
(( (8#$mode & 0022) == 0 )) || zr_die 'Каталог сертификата доступен для записи другим пользователям; выберите защищённый каталог'
key=$dir/zarnitsa.key
cert=$dir/zarnitsa.crt
for path in "$key" "$cert"; do
    if [[ -e $path || -L $path ]]; then
        [[ -f $path && ! -L $path && $(stat -c '%u' -- "$path") == "$EUID" ]] || zr_die "Не перезаписываю чужой файл, ссылку или каталог: $path"
    fi
done
if [[ -e $key || -e $cert ]]; then
    printf 'Уже существуют ключ/сертификат в %s. Старый сертификат перестанет соответствовать новому ключу у клиентов.\n' "$dir"
    if (( ! yes )); then
        zr_confirm 'Заменить существующую пару?' || {
            rc=$?; printf 'Отменено, существующие файлы сохранены.\n'
            if (( rc == 1 )); then exit 0; else exit 1; fi
        }
    fi
fi

tmp=$(mktemp -d "$dir/.zarnitsa-cert.XXXXXX")
key_written=0; cert_written=0; complete=0
cleanup() {
    local rc=$?
    if (( ! complete )); then
        if (( key_written )); then
            if [[ -f $tmp/old.key ]]; then mv -f -- "$tmp/old.key" "$key"; else rm -f -- "$key"; fi
        fi
        if (( cert_written )); then
            if [[ -f $tmp/old.crt ]]; then mv -f -- "$tmp/old.crt" "$cert"; else rm -f -- "$cert"; fi
        fi
    fi
    rm -rf -- "$tmp"
    return "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
if (( ${#names[@]} )); then cn=${names[0]}; else cn=${ips[0]}; fi
{
    printf '[req]\nprompt = no\ndistinguished_name = dn\nx509_extensions = server\n[dn]\nCN = %s\n' "$cn"
    printf '[server]\nbasicConstraints = critical,CA:FALSE\nkeyUsage = critical,digitalSignature\nextendedKeyUsage = serverAuth\nsubjectAltName = @san\n[san]\n'
    i=0; for ip in "${ips[@]}"; do i=$((i + 1)); printf 'IP.%s = %s\n' "$i" "$ip"; done
    i=0; for name in "${names[@]}"; do i=$((i + 1)); printf 'DNS.%s = %s\n' "$i" "$name"; done
} > "$tmp/openssl.cnf"
"${openssl_cmd[@]}" req -new -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -sha256 \
    -days "$days" -config "$tmp/openssl.cnf" -extensions server -keyout "$tmp/new.key" -out "$tmp/new.crt" >/dev/null
"${openssl_cmd[@]}" pkey -in "$tmp/new.key" -noout >/dev/null
"${openssl_cmd[@]}" x509 -in "$tmp/new.crt" -noout >/dev/null
chmod 0600 -- "$tmp/new.key"
chmod 0644 -- "$tmp/new.crt"
[[ ! -f $key ]] || cp -p -- "$key" "$tmp/old.key"
[[ ! -f $cert ]] || cp -p -- "$cert" "$tmp/old.crt"
mv -Tf -- "$tmp/new.key" "$key"; key_written=1
mv -Tf -- "$tmp/new.crt" "$cert"; cert_written=1
complete=1
printf 'Готово. Ключ: %s (0600)\nСертификат: %s\n' "$key" "$cert"
printf 'Сертификат самоподписанный: доверьте его клиентам явно. API/конфиг Зарницы, firewall и внешние порты не менялись.\n'
