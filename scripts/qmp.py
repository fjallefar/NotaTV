#!/usr/bin/env python3
"""Styr QEMU via QMP: screenshots og tastetryk.
Brug: qmp.py <sock> shot fil.png | key down [down ok ...] | type tekst | quit"""
import json, socket, sys, time, os
from PIL import Image

def qmp(sock_path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    f = s.makefile('rw')
    f.readline()
    def cmd(name, **args):
        f.write(json.dumps({"execute": name, "arguments": args} if args else {"execute": name}) + "\n")
        f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r:
                return r
    cmd("qmp_capabilities")
    return cmd

KEYMAP = {"ok": "ret", "back": "esc", "home": "home", "menu": "f10", "info": "f1", "yellow": "f4",
          "red": "f2", "bs": "backspace", "pgup": "pgup", "pgdn": "pgdn"}

def main():
    sock, op, *rest = sys.argv[1:]
    cmd = qmp(sock)
    if op == "shot":
        ppm = rest[0] + ".ppm"
        cmd("screendump", filename=os.path.abspath(ppm))
        Image.open(ppm).save(rest[0]); os.remove(ppm)
    elif op == "key":
        for k in rest:
            if k.startswith("sleep"):
                time.sleep(float(k[5:] or 1)); continue
            cmd("send-key", keys=[{"type": "qcode", "data": KEYMAP.get(k, k)}])
            time.sleep(0.25)
    elif op == "type":
        for ch in " ".join(rest):
            q = {" ": "spc", ".": "dot", "/": "slash", ":": "shift-semicolon", "-": "minus"}.get(ch, ch.lower())
            keys = [{"type": "qcode", "data": "shift"}, {"type": "qcode", "data": "semicolon"}] if q == "shift-semicolon" else [{"type": "qcode", "data": q}]
            cmd("send-key", keys=keys); time.sleep(0.08)
    elif op == "quit":
        cmd("quit")

main()
