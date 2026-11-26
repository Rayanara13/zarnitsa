#!/bin/bash
# Интеграционная проверка цепочки ICMP так, как на Astra: ping_group_range = "1 0", пользователь astra без прав.
# Запуск: docker run --sysctl net.ipv4.ping_group_range="1 0" … bash /src/docker/itest-icmp.sh
set -u
B=/build/full
W=$(mktemp -d /tmp/zr-it.XXXXXX)
PROGRAM=$(mktemp -d /opt/zr-icmp-it.XXXXXX)
chmod 0755 "$PROGRAM"
mkdir -m 0755 "$PROGRAM/bin"
cp "$B/zarnitsa" "$PROGRAM/bin/zarnitsa"
chmod 0755 "$PROGRAM/bin/zarnitsa"
chown -R root:root "$PROGRAM"
chown astra:astra "$W"
fail=0
check() { if grep -q -- "$2" "$1"; then echo "  ок: $3"; else echo "  ПРОВАЛ: $3"; fail=1; fi; }

echo "ping_group_range: $(sysctl -n net.ipv4.ping_group_range | tr '\t' ' ')"

run() {   # $1 — метка, дальше аргументы zarnitsa
  local tag=$1; shift
  su astra -c "cd $W && QT_QPA_PLATFORM=offscreen timeout 9 $PROGRAM/bin/zarnitsa --headless --config $W/$tag.json $*" > "$W/$tag.out" 2>&1
}

echo "1) нет помощника — должен быть системный ping"
run t1 --no-api -i 1 127.0.0.1 10.255.255.1
check "$W/t1.out" "ICMP — системный ping" "выбран системный ping"
check "$W/t1.out" "10.255.255.1.*Нет связи" "недоступный адрес ушёл в «Нет связи»"

echo "2) помощник рядом, но без cap_net_raw — откат на системный ping"
cp "$B/zarnitsa-icmp" "$PROGRAM/bin/zarnitsa-icmp"
chown root:root "$PROGRAM/bin/zarnitsa-icmp"
chmod 0755 "$PROGRAM/bin/zarnitsa-icmp"
run t2 --no-api -i 1 127.0.0.1
check "$W/t2.out" "ICMP: системный ping" "после отказа помощника — системный ping"

echo "3) помощник с cap_net_raw — быстрый путь"
# Даже в тесте filecap выдаётся только root-owned helper под защищённым /opt.
setcap cap_net_raw+ep "$PROGRAM/bin/zarnitsa-icmp"
run t3 --api 127.0.0.1:18790 -i 1 127.0.0.1 10.255.255.1 &
sleep 5
TOKEN=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["settings"]["api_token"])' "$W/t3.json")
curl -s http://127.0.0.1:18790/healthz > "$W/health.json"
curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:18790/api/v1/targets > "$W/targets.json"
curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:18790/api/v1/targets > "$W/noauth.code"
wait
check "$W/t3.out" "ICMP: помощник" "помощник поднялся"
check "$W/health.json" '"status":"ok"' "/healthz отвечает"
check "$W/targets.json" '"status":"up"' "API отдаёт цель в норме"
check "$W/targets.json" '"status":"down"' "API отдаёт лежащую цель"
check "$W/noauth.code" "401" "без токена — 401"
grep -q "zarnitsa-icmp" <(ps -eo comm) && { echo "  ПРОВАЛ: помощник остался после выхода"; fail=1; } || echo "  ок: помощник завершился вместе с программой"

echo "== вывод t3:"; sed -n '1,12p' "$W/t3.out"
exit $fail
