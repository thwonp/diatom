#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Handover + resident test (plorpos-gkd.72.2): plays the launcher's part against
# a resident diatom on its own socket. Per cycle: RUN a game, PAUSE (window
# must unmap: glass shows the launcher), RESUME (window must map again and
# show the game, not black, not the launcher), STOP. Times each reply, grabs
# the glass with grim, and samples the resident's RSS.
#   gkd-handover.py <diatom binary> <cycles>   (on the GKD, launcher on the shelf,
#   WAYLAND_DISPLAY/XDG_RUNTIME_DIR as diatom has them; needs grim)
import os, socket, subprocess, sys, time

BIN, CYCLES = sys.argv[1], int(sys.argv[2])
SOCK = "/tmp/hotest.sock"
C = "/storage/games-external/TortOS/cores"
R = "/storage/games-external/Roms"
GAMES = [
    ("mgba_libretro.so", R + "/Game Boy Advance/Advance Wars.gba"),
    ("genesis_plus_gx_libretro.so", R + "/Genesis/Addams Family, The.md"),
    ("fbneo_libretro.so", R + "/Arcade/1942.zip"),
]

def grab():
    """The glass as RGB bytes, sampled on a 40px grid."""
    subprocess.run(["grim", "-t", "ppm", "/tmp/ho.ppm"], check=True)
    d = open("/tmp/ho.ppm", "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    return [px[(y * w + x) * 3:(y * w + x) * 3 + 3] for y in range(20, h, 40) for x in range(20, w, 40)]

def same(a, b):
    return sum(1 for p, q in zip(a, b) if p == q) / len(a)

def black(a):
    return sum(1 for p in a if p == b"\0\0\0") / len(a)

def rss(pid):
    for l in open("/proc/%d/status" % pid):
        if l.startswith("VmRSS:"):
            return int(l.split()[1]) // 1024
    return -1

class Plane:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX)
        for _ in range(100):
            try:
                self.s.connect(SOCK); break
            except OSError:
                time.sleep(0.1)
        self.buf = b""
    def send(self, line):
        self.s.sendall(line.encode() + b"\n")
    def wait(self, prefix, timeout=15):
        end = time.time() + timeout
        self.s.settimeout(0.2)
        while time.time() < end:
            while b"\n" in self.buf:
                l, self.buf = self.buf.split(b"\n", 1)
                if l.decode().startswith(prefix):
                    return l.decode()
            try:
                self.buf += self.s.recv(4096)
            except socket.timeout:
                pass
        return None

def ms(t):
    return "%5.0f" % ((time.time() - t) * 1000)

launcher = grab()   # what is on glass with no diatom window: the shelf
try: os.unlink(SOCK)
except OSError: pass
os.makedirs("/tmp/hosave", exist_ok=True)
proc = subprocess.Popen([BIN, "--socket", SOCK, "--cores", C, "--save", "/tmp/hosave"],
                        stdout=open("/tmp/ho-diatom.log", "w"), stderr=subprocess.STDOUT)
p = Plane()
fails = 0
print("cycle game                 run  pause resume  stop | game-on-glass  paused=shelf  resumed  rss")
rss0 = None
for i in range(CYCLES):
    core, rom = GAMES[i % len(GAMES)]
    t = time.time(); p.send("RUN\tcore=%s/%s\trom=%s\ttag=T\tsave=/tmp/hosave" % (C, core, rom))
    ok_run = p.wait("RUNNING"); t_run = ms(t)
    time.sleep(2.0)
    g1 = grab()
    t = time.time(); p.send("PAUSE"); ok_p = p.wait("PAUSED"); t_pause = ms(t)
    # grim takes ~220 ms on the GKD and sway's screencopy can hand back its
    # last composited frame, so "the shelf is up" is checked as "within 2 s",
    # not at a fixed instant - it cannot resolve the handover's real latency.
    t0 = time.time(); g2 = grab()
    while same(g2, launcher) <= 0.9 and time.time() - t0 < 2.0: g2 = grab()
    if os.environ.get("HO_PROBE"):
        # When does the shelf reach the glass? Grab until it does; each
        # grab's own time is reported so it can be taken off.
        t0 = time.time(); seen = None; grabs = []
        while time.time() - t0 < 2.0:
            tg = time.time(); g = grab(); grabs.append(time.time() - tg)
            if same(g, launcher) > 0.9: seen = time.time() - t0; break
        print("   pause->shelf on glass: %s ms (one grim takes %.0f ms)" % (
            "%.0f" % (seen * 1000) if seen else ">2000", 1000 * sum(grabs) / len(grabs)))
    t = time.time(); p.send("RESUME"); ok_r = p.wait("RUNNING"); t_res = ms(t)
    time.sleep(0.15)
    g3 = grab()
    t = time.time(); p.send("STOP"); ok_s = p.wait("EXIT"); t_stop = ms(t)
    r = rss(proc.pid)
    if i == 2: rss0 = r
    game = same(g1, launcher) < 0.5 and black(g1) < 0.98
    shelf = same(g2, launcher) > 0.9
    resumed = same(g3, launcher) < 0.5 and black(g3) < 0.98
    bad = not (ok_run and ok_p and ok_r and ok_s and game and shelf and resumed)
    fails += bad
    print("%5d %-18s %s %s %s %s | %-13s  %-12s  %-7s  %d MB%s" % (
        i, os.path.basename(rom)[:18], t_run, t_pause, t_res, t_stop,
        "yes" if game else "NO", "yes" if shelf else "NO", "yes" if resumed else "NO",
        r, "   <- FAIL" if bad else ""), flush=True)
p.send("QUIT")
proc.wait(timeout=10)
print("rss after cycle 2: %d MB, at the end: %d MB" % (rss0, r))
print("HANDOVER: %s" % ("all passed" if not fails else "%d cycle(s) FAILED" % fails))
