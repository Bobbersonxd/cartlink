#!/usr/bin/env python3
# End-to-end check: quitting RetroArch while "Load from URL" is still
# downloading must exit cleanly (no crash, no AddressSanitizer report).
#
# RetroArch runs inside a headless gamescope (no window on the desktop),
# with a throwaway HOME and config, null audio. The menu is driven with
# network commands; the link is typed with XTest into gamescope's own
# Xwayland display (the script refuses to type if that display is the
# desktop's). serve.py's /slow route sends 64 MiB slowly, so the
# download is still running when QUIT arrives.
#
# Usage: tools/url_e2e_quit.py [<retroarch binary>]
#   default binary: ./retroarch in the repo. An ASan build
#   (make SANITIZER=address) also has its report checked.
# Needs: gamescope, python3-xlib, iproute2 (ss).

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RA = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else ROOT + "/retroarch"
SERVE = ROOT + "/tests-other/url_content/serve.py"
NICE = ["nice", "-n", "19", "ionice", "-c3"]


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def wait_port(port, tries=200):
    for _ in range(tries):
        try:
            socket.create_connection(("127.0.0.1", port), 1).close()
            return True
        except OSError:
            time.sleep(0.05)
    return False


def established(port):
    """Open TCP connections to port (the download in progress)."""
    out = subprocess.run(["ss", "-tnH", "state", "established",
                          "( dport = :%d )" % port],
                         capture_output=True, text=True).stdout
    return len([l for l in out.splitlines() if l.strip()])


def main():
    if not os.access(RA, os.X_OK):
        print("No RetroArch binary at %s" % RA)
        return 1
    work = tempfile.mkdtemp(prefix="url_e2e_quit.")
    home = work + "/home"
    for d in ("home", "assets", "saves", "system", "cache"):
        os.makedirs(work + "/" + d)
    cmd_port = free_port()
    web_port = free_port()
    with open(work + "/retroarch.cfg", "w") as f:
        f.write("""video_driver = "vulkan"
audio_driver = "null"
audio_enable = "false"
microphone_enable = "false"
menu_driver = "rgui"
input_joypad_driver = "null"
network_cmd_enable = "true"
network_cmd_port = "%d"
config_save_on_exit = "false"
video_fullscreen = "false"
pause_nonactive = "false"
quit_press_twice = "false"
threaded_data_runloop_enable = "true"
history_list_enable = "false"
core_assets_directory = "%s/assets"
savefile_directory = "%s/saves"
savestate_directory = "%s/saves"
system_directory = "%s/system"
cache_directory = "%s/cache"
""" % (cmd_port, work, work, work, work, work))

    procs = []
    fails = 0
    try:
        web = subprocess.Popen(NICE + ["python3", SERVE, str(web_port)],
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        procs.append(web)
        if not wait_port(web_port):
            print("FAIL: serve.py did not start")
            return 1

        env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=home + "/.config",
                   ASAN_OPTIONS="detect_leaks=0:log_path=%s/asan" % work)
        inner = ("env > {w}/gsenv.txt; {ra} --config={w}/retroarch.cfg "
                 "--verbose --log-file={w}/ra.log; echo $? > {w}/rc"
                 ).format(w=work, ra=RA)
        gs = subprocess.Popen(NICE + ["gamescope", "--backend", "headless",
                              "-w", "1280", "-h", "720", "-W", "1280",
                              "-H", "720", "--", "bash", "-c", inner],
                              env=env, stdout=open(work + "/gs.log", "w"),
                              stderr=subprocess.STDOUT)
        procs.append(gs)
        for _ in range(100):
            if os.path.exists(work + "/gsenv.txt"):
                break
            time.sleep(0.1)
        time.sleep(5)
        genv = dict(l.rstrip("\n").split("=", 1)
                    for l in open(work + "/gsenv.txt") if "=" in l)
        gdisp = genv.get("DISPLAY")
        if not gdisp or gdisp == os.environ.get("DISPLAY"):
            print("FAIL: no separate gamescope display, refusing to type")
            return 1

        from Xlib import X, XK, display
        from Xlib.ext import xtest
        d = display.Display(gdisp)
        special = {":": "colon", "/": "slash", ".": "period"}

        def key(name):
            ks = XK.string_to_keysym(special.get(name, name))
            kc, idx = list(d.keysym_to_keycodes(ks))[0]
            shift = d.keysym_to_keycode(XK.XK_Shift_L)
            if idx & 1:
                xtest.fake_input(d, X.KeyPress, shift)
            xtest.fake_input(d, X.KeyPress, kc)
            d.sync()
            time.sleep(0.03)
            xtest.fake_input(d, X.KeyRelease, kc)
            d.sync()
            time.sleep(0.03)
            if idx & 1:
                xtest.fake_input(d, X.KeyRelease, shift)
                d.sync()

        def cmd(c):
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            for _ in range(8):
                s.sendto(c.encode(), ("127.0.0.1", cmd_port))
                time.sleep(0.002)
            time.sleep(0.8)

        # Main Menu > Load Content > Load from URL, type the link.
        cmd("MENU_DOWN")
        cmd("MENU_B")
        cmd("MENU_B")
        for c in "http://127.0.0.1:%d/slow" % web_port:
            key(c)
        key("Return")

        busy = 0
        for _ in range(50):
            busy = established(web_port)
            if busy:
                break
            time.sleep(0.1)
        time.sleep(2)
        busy = established(web_port)
        if busy:
            print("PASS: download running (%d connection) when QUIT is sent"
                  % busy)
        else:
            print("FAIL: no download running before QUIT")
            fails += 1

        cmd("QUIT")
        rc = None
        for _ in range(300):
            if os.path.exists(work + "/rc"):
                try:
                    rc = int(open(work + "/rc").read().strip() or "-1")
                    break
                except ValueError:
                    pass
            time.sleep(0.1)

        asan = [f for f in os.listdir(work) if f.startswith("asan")]
        asan_text = "".join(open(work + "/" + f).read() for f in asan)
        finished = [f for f in os.listdir(work + "/assets/url_cache")
                    if not f.endswith(".part")] \
            if os.path.isdir(work + "/assets/url_cache") else []
        if rc == 0:
            print("PASS: RetroArch exited with 0 after QUIT mid-download")
        else:
            print("FAIL: exit status %s (see %s)" % (rc, work))
            fails += 1
        if "AddressSanitizer" in asan_text:
            print("FAIL: AddressSanitizer report:")
            print(asan_text[:3000])
            fails += 1
        else:
            print("PASS: no AddressSanitizer report%s"
                  % ("" if asan or "asan" in RA else
                     " (binary may not be an ASan build)"))
        if finished:
            print("FAIL: download finished before QUIT: %s" % finished)
            fails += 1
        if not ("[URL]" in open(work + "/ra.log").read()
                or os.path.isdir(work + "/assets/url_cache")):
            print("note: no [URL] line in the log (normal for a menu"
                  " download that never finished)")
    finally:
        for p in reversed(procs):
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
    if fails:
        print("url_e2e_quit: %d check(s) failed, work dir kept: %s"
              % (fails, work))
        return 1
    print("url_e2e_quit: all checks passed")
    shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
