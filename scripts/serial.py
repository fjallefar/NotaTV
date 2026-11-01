#!/usr/bin/env python3
"""Kør en kommando på NotaTV' serielle konsol i QEMU og udskriv svaret.
Brug: serial.py <sock> '<kommando>' [timeout]   (logger ind som root automatisk)"""
import socket, sys, time

sock, cmd = sys.argv[1], sys.argv[2]
timeout = float(sys.argv[3]) if len(sys.argv) > 3 else 15
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock)
s.settimeout(0.3)

def read_until(markers, t):
    buf, end = b"", time.time() + t
    while time.time() < end:
        try:
            d = s.recv(65536)
            if d: buf += d
        except socket.timeout:
            pass
        if any(m in buf for m in markers):
            break
    return buf

s.send(b"\n")
b = read_until([b"login:", b"# "], 5)
if b"login:" in b:
    s.send(b"root\n"); read_until([b"assword:"], 5)
    s.send(b"notatv\n"); read_until([b"# "], 5)
s.send(b"stty -echo 2>/dev/null; echo __S''__; " + cmd.encode() + b"; echo __E''__\n")
out = read_until([b"__E__\r\n", b"__E__\n"], timeout).decode(errors="replace")
if "__S__" in out:
    out = out.split("__S__", 1)[1]
print(out.replace("__E__", "").strip())
