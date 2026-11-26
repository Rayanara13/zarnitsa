#!/bin/bash
# Живая проверка связки: настоящий Иней 1.5 из его офлайн-комплекта и настоящая Роса из её ZIP,
# всё на loopback внутри контейнера Astra. Комплекты лежат в /kits (inei.zip, rosa.zip).
set -u
B=/build/full
P=$B/tests/zr-import-probe
W=/tmp/src-it
rm -rf "$W"; mkdir -p "$W"
fail=0
check() { if grep -q -- "$2" "$1"; then echo "  ок: $3"; else echo "  ПРОВАЛ: $3"; fail=1; fi; }
unz() { python3 -I -c 'import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$1" "$2"; }
wait_url() { for _ in $(seq 1 60); do curl -sk -o /dev/null --max-time 2 "$1" && return 0; sleep 1; done; return 1; }

echo "== Иней 1.5 (офлайн-комплект linux x64)"
mkdir -p "$W/inei"; unz /kits/inei.zip "$W/inei"
cd "$W/inei/MonitoringSIP"
find . -name '*.sh' -exec chmod +x {} +; find runtime -path '*/bin/*' -type f -exec chmod +x {} + 2>/dev/null
MONITORING_SIP_ROLE=standalone MONITORING_SIP_HOST=127.0.0.1 MONITORING_SIP_PORT=18000 nohup ./start_linux.sh > "$W/inei.log" 2>&1 &
if ! wait_url http://127.0.0.1:18000/login; then echo "  ПРОВАЛ: Иней не поднялся"; tail -20 "$W/inei.log"; exit 1; fi
FS=$(find "$W/inei" -name first-start.txt | head -1)
PASS=$(sed -n 's/^[^:]*[Пп]ароль[^:]*:[[:space:]]*//p' "$FS" | head -1 | tr -d '\r ')
[ -n "$PASS" ] || { echo "  ПРОВАЛ: нет пароля в first-start.txt"; sed 's/:.*/: …/' "$FS"; exit 1; }
curl -s -c "$W/cj" -H 'Content-Type: application/json' -d "{\"username\":\"admin\",\"password\":\"$PASS\"}" http://127.0.0.1:18000/login > "$W/inei-login.json"
for card in '{"number":"101004001","name":"Приёмная","ip":"10.1.1.21"}' '{"number":"101004002","name":"Бухгалтерия","ip":"10.1.1.22"}' '{"number":"101004003","name":"Склад","ip":"мусор"}'; do
  curl -s -b "$W/cj" -H 'Content-Type: application/json' -d "$card" http://127.0.0.1:18000/add_phone >> "$W/inei-add.json"
done
check "$W/inei-login.json" '"ok":true\|"ok": true' "вход в Иней curl'ом (подготовка)"
"$P" inei http://127.0.0.1:18000 admin "$PASS" > "$W/inei-import.json" 2>&1; echo "  код импорта: $?"
check "$W/inei-import.json" '"ok": true' "Зарница вошла в Иней и прочитала карточки"
check "$W/inei-import.json" '"name": "Приёмная · 101004001"' "карточка с именем и номером"
check "$W/inei-import.json" '"address": "10.1.1.22"' "вторая карточка"
check "$W/inei-import.json" 'Номер 101004003: нет корректного IP' "карточка с мусорным IP отброшена с предупреждением"
"$P" inei http://127.0.0.1:18000 admin неверный > "$W/inei-bad.json" 2>&1
check "$W/inei-bad.json" '7 неудач' "неверный пароль — понятное сообщение про блокировку Инея"
grep -c 'HTTP_REQ' "$W/inei/MonitoringSIP/app/users_data/app.log" > "$W/inei-reqs.txt" 2>/dev/null || true

echo "== Роса (Rosa-Video-Timeout-2026-11-25)"
mkdir -p "$W/rosa"; unz /kits/rosa.zip "$W/rosa"
cd "$W/rosa"; chmod +x bin/rosa-server-linux-amd64
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 90 -subj /CN=rosa-test \
  -addext "subjectAltName=IP:127.0.0.1" -keyout key.pem -out cert.pem >/dev/null 2>&1
ROSA_ADDR=127.0.0.1:18443 ROSA_TLS_CERT=$PWD/cert.pem ROSA_TLS_KEY=$PWD/key.pem ROSA_DB=$W/rosa/rosa.db \
ROSA_ADMIN_PASSWORD=ZarnitsaTest-2026 ROSA_SIP_ADDR=127.0.0.1:15060 nohup ./bin/rosa-server-linux-amd64 > "$W/rosa.log" 2>&1 &
if ! wait_url https://127.0.0.1:18443/healthz; then echo "  ПРОВАЛ: Роса не поднялась"; tail -20 "$W/rosa.log"; exit 1; fi
TOKEN=$(curl -sk -H 'Content-Type: application/json' -d '{"username":"admin","password":"ZarnitsaTest-2026"}' https://127.0.0.1:18443/api/v1/sessions | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["token"])')
UID1=$(curl -sk -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d '{"username":"phone1001","password":"Phone1001-Secret!"}' https://127.0.0.1:18443/api/v1/admin/users | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["id"])')
curl -sk -X PUT -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' -d "{\"user_id\":\"$UID1\",\"password\":\"SipSecret-1001-xyz\"}" https://127.0.0.1:18443/api/v1/admin/sip/accounts/1001 > /dev/null
curl -sk -X DELETE -H "Authorization: Bearer $TOKEN" https://127.0.0.1:18443/api/v1/session > /dev/null
python3 -I /src/tests/sip_register.py 127.0.0.1 15060 1001 SipSecret-1001-xyz > "$W/sip-reg.txt" 2>&1
check "$W/sip-reg.txt" "SIP/2.0 200" "тестовый телефон 1001 зарегистрировался в Росе (подготовка)"
FP=$(openssl x509 -in cert.pem -outform der | sha256sum | cut -d' ' -f1)
"$P" rosa https://127.0.0.1:18443 admin ZarnitsaTest-2026 --pin auto --mode sip > "$W/rosa-import.json" 2>&1; echo "  код импорта: $?"
check "$W/rosa-import.json" "\"sha256\": \"$FP\"" "отпечаток сертификата совпал с openssl"
check "$W/rosa-import.json" '"trusted": false' "самоподписанный сертификат распознан"
check "$W/rosa-import.json" '"source_key": "sip/1001"' "SIP-абонент 1001 из registrations"
check "$W/rosa-import.json" '"address": "https://127.0.0.1:18443/healthz"' "сам сервер Росы — /healthz"
check "$W/rosa-import.json" '"source_key": "server/sip"' "SIP Росы — OPTIONS на её порт"
"$P" rosa https://127.0.0.1:18443 admin ZarnitsaTest-2026 --pin 00ff00ff > "$W/rosa-badpin.json" 2>&1
check "$W/rosa-badpin.json" 'не совпал' "чужой отпечаток — отказ"
"$P" rosa https://127.0.0.1:18443 admin неверный-пароль-123 --pin "$FP" > "$W/rosa-badpass.json" 2>&1
check "$W/rosa-badpass.json" 'Неверный логин или пароль' "неверный пароль Росы"

echo "== Зарница следит за обоими сервером без окна"
cd "$W"
QT_QPA_PLATFORM=offscreen timeout 14 "$B/zarnitsa" --headless --config "$W/z.json" --api 127.0.0.1:18790 -i 1 > "$W/z.out" 2>&1 &
ZPID=$!
sleep 3
ZT=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1]))["settings"]["api_token"])' "$W/z.json")
curl -s -H "Authorization: Bearer $ZT" -H 'Content-Type: application/json' -d "[{\"line\":\"https://127.0.0.1:18443/healthz\",\"name\":\"Роса\",\"tls_pin\":\"$FP\"},{\"line\":\"sip:127.0.0.1:15060\",\"name\":\"Роса SIP\"},{\"line\":\"http://127.0.0.1:18000/login\",\"name\":\"Иней\"},{\"line\":\"https://127.0.0.1:18443/healthz\",\"name\":\"Роса без закрепления\"}]" http://127.0.0.1:18790/api/v1/targets > /dev/null
sleep 6
curl -s -H "Authorization: Bearer $ZT" http://127.0.0.1:18790/api/v1/targets > "$W/z-targets.json"
wait "$ZPID"
python3 -I - "$W/z-targets.json" <<'EOF'
import json, sys
ts = {t["name"]: t for t in json.load(open(sys.argv[1]))}
for name, want in [("Роса", "up"), ("Роса SIP", "up"), ("Иней", "up"), ("Роса без закрепления", "down")]:
    t = ts.get(name, {})
    got = t.get("status")
    print("  %s: %s %s (%s)" % ("ок" if got == want else "ПРОВАЛ", name, got, t.get("detail") or t.get("last_error") or ""))
    if got != want: sys.exit(1)
EOF
[ $? -eq 0 ] || fail=1

pkill -f 'rosa-server-linux-amd6[4]' ; pkill -f 'run.p[y]'; pkill -f 'server.p[y]'
exit $fail
