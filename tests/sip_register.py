#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Регистрация тестового SIP-абонента с digest MD5 — чтобы у Росы появилась запись в registrations.
# Python 3.7 из Astra 1.7, только стандартная библиотека.
# Аргументы: <хост> <порт> <номер> <пароль>
import hashlib
import re
import secrets
import socket
import sys

host, port, number, password = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", 0))
sock.settimeout(3)
lport = sock.getsockname()[1]
call_id = secrets.token_hex(8)
tag = secrets.token_hex(4)
uri = "sip:%s:%d" % (host, port)


def md5(s):
    return hashlib.md5(s.encode()).hexdigest()


def send(cseq, auth=None):
    lines = [
        "REGISTER %s SIP/2.0" % uri,
        "Via: SIP/2.0/UDP 127.0.0.1:%d;branch=z9hG4bK%s;rport" % (lport, secrets.token_hex(6)),
        "Max-Forwards: 70",
        "From: <sip:%s@%s>;tag=%s" % (number, host, tag),
        "To: <sip:%s@%s>" % (number, host),
        "Call-ID: %s" % call_id,
        "CSeq: %d REGISTER" % cseq,
        "Contact: <sip:%s@127.0.0.1:%d>" % (number, lport),
        "Expires: 300",
    ]
    if auth:
        lines.append(auth)
    lines += ["Content-Length: 0", "", ""]
    sock.sendto("\r\n".join(lines).encode(), (host, port))
    while True:
        data, _ = sock.recvfrom(65535)
        text = data.decode(errors="replace")
        if not text.startswith("SIP/2.0 1"):   # пропускаем 100 Trying
            return text


resp = send(1)
if resp.split("\r\n")[0].split(" ")[1] == "401":
    challenges = re.findall(r"^WWW-Authenticate:\s*Digest\s+(.*)$", resp, re.I | re.M)
    chosen = None
    for ch in challenges:
        params = dict((k, v.strip('"')) for k, v in re.findall(r'(\w+)=("[^"]*"|[^,\s]+)', ch))
        if params.get("algorithm", "MD5").upper() == "MD5":
            chosen = params
            break
    if chosen is None:
        print("нет MD5-вызова: %r" % challenges)
        sys.exit(2)
    realm, nonce, qop = chosen["realm"], chosen["nonce"], chosen.get("qop")
    ha1 = md5("%s:%s:%s" % (number, realm, password))
    ha2 = md5("REGISTER:%s" % uri)
    if qop:
        nc, cnonce = "00000001", secrets.token_hex(8)
        r = md5("%s:%s:%s:%s:auth:%s" % (ha1, nonce, nc, cnonce, ha2))
        auth = ('Authorization: Digest username="%s", realm="%s", nonce="%s", uri="%s", response="%s", '
                'algorithm=MD5, qop=auth, nc=%s, cnonce="%s"' % (number, realm, nonce, uri, r, nc, cnonce))
    else:
        r = md5("%s:%s:%s" % (ha1, nonce, ha2))
        auth = ('Authorization: Digest username="%s", realm="%s", nonce="%s", uri="%s", response="%s", algorithm=MD5'
                % (number, realm, nonce, uri, r))
    resp = send(2, auth)
print(resp.split("\r\n")[0])
sys.exit(0 if " 200 " in resp.split("\r\n")[0] + " " else 1)
