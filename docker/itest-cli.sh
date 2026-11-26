#!/bin/bash
# Командная строка проверяется отдельными процессами, как при запуске службы и окна.
set -euo pipefail
B=${1:-/build/full/zarnitsa}
W=$(mktemp -d /tmp/zarnitsa-cli.XXXXXX)
PID=
cleanup() {
    if [[ -n $PID ]] && kill -0 "$PID" 2>/dev/null; then kill -TERM "$PID"; wait "$PID" || :; fi
}
trap cleanup EXIT
export QT_QPA_PLATFORM=offscreen
export LC_ALL=C.UTF-8

wait_api() {
    for _ in {1..80}; do
        if curl --silent --fail --max-time 1 "http://127.0.0.1:$1/healthz" > /dev/null; then return 0; fi
        sleep 0.1
    done
    printf 'API не запустился: %s\n' "$1" >&2
    return 1
}
stop() { kill -TERM "$PID"; wait "$PID"; PID=; }

"$B" --headless --config "$W/z.json" --api 127.0.0.1:18791 -i 0.1 -w 250 127.0.0.1 > "$W/headless.out" 2>&1 &
PID=$!
wait_api 18791
sleep 3
python3 -I - "$W" <<'PY'
import json, os, sys, time, urllib.request
w = sys.argv[1]
for _ in range(30):
    try:
        with open(os.path.join(w, 'z.json')) as f:
            config = json.load(f)
        break
    except (FileNotFoundError, ValueError):
        time.sleep(.1)
s = config['settings']
token = s['api_token']
with open(os.path.join(w, 'headless.out')) as f:
    output = f.read()
assert token not in output, 'API token leaked to stdout'
assert s['interval_ms'] == 1000 and s['timeout_ms'] == 1000, 'CLI overrides changed saved settings'
target = config['targets'][0]['id']
req = urllib.request.Request('http://127.0.0.1:18791/api/v1/targets/' + target,
                             headers={'Authorization': 'Bearer ' + token})
with urllib.request.urlopen(req, timeout=3) as response:
    result = json.load(response)
assert result['sent'] >= 8, 'headless applySettings lost CLI interval: %r' % result
assert result['status'] == 'up', result
print('ок: CLI-интервал применяется, базовые настройки не изменены, токен не попал в stdout')
PY
stop

"$B" --headless --config "$W/z.json" --api 127.0.0.1:18791 --print-api-token > "$W/explicit-token.out" 2>&1 &
PID=$!
wait_api 18791
python3 -I - "$W" <<'PY'
import json, os, sys
w = sys.argv[1]
with open(os.path.join(w, 'z.json')) as f:
    token = json.load(f)['settings']['api_token']
with open(os.path.join(w, 'explicit-token.out')) as f:
    assert 'Токен API: ' + token in f.read()
print('ок: токен выводится только по явному --print-api-token')
PY
stop

"$B" --config "$W/gui.json" --api 127.0.0.1:18792 --no-single --size 760x620 \
    --screenshot "$W/gui.png" --screenshot-delay 2500 > "$W/gui.out" 2>&1 &
PID=$!
wait_api 18792
wait "$PID"; PID=
test -s "$W/gui.png"
printf 'ок: --api включает API и в графическом режиме; PNG узкого окна записан\n'

"$B" --config "$W/gui.json" --no-api --no-single --screenshot "$W/no-api.png" --screenshot-delay 1500 > "$W/no-api.out" 2>&1 &
PID=$!
sleep 0.6
if curl --silent --fail --max-time 1 http://127.0.0.1:18792/healthz > /dev/null; then
    printf 'ПРОВАЛ: --no-api проигнорирован в окне\n' >&2; exit 1
fi
wait "$PID"; PID=
printf 'ок: --no-api отключает ранее сохранённый API окна\n'

set +e
"$B" --demo --screenshot "$W/absent/file.png" --screenshot-delay 10 > "$W/failed-shot.out" 2>&1
RC=$?
set -e
test "$RC" -eq 1
printf 'ок: ошибка записи PNG возвращает ненулевой код\n'

"$B" --headless --config "$W/z.json" --no-api > "$W/headless-no-api.out" 2>&1 &
PID=$!
sleep 0.6
if curl --silent --fail --max-time 1 http://127.0.0.1:18791/healthz > /dev/null; then
    printf 'ПРОВАЛ: --no-api проигнорирован без окна\n' >&2; exit 1
fi
stop
printf 'ок: --no-api работает без окна; SIGTERM завершает процесс с кодом 0\n'
printf 'CLI: все проверки пройдены. Временные файлы: %s\n' "$W"
