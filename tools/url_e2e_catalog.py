#!/usr/bin/env python3
# End-to-end check of the "Load from URL" game list menus: a folder
# link opens a game list, folders open one level deeper, Back goes up,
# a file is downloaded, an empty folder says so, the server is saved
# under Saved Servers (and can be removed), and a login typed for a
# list is saved and reused one level deeper without asking again.
#
# RetroArch runs inside a headless gamescope (no window on the desktop),
# with a throwaway HOME and config, null audio. The menu is driven with
# network commands; links and logins are typed with XTest into
# gamescope's own Xwayland display (the script refuses to type if that
# display is the desktop's). gamescopectl (this gamescope's socket
# only) saves a screenshot of each step into the shots folder.
#
# Usage: tools/url_e2e_catalog.py [<retroarch binary> [<shots dir>]]
#   An ASan binary (make SANITIZER=address) also has its report checked.
#   default binary: ./retroarch in the repo; shots are only kept when
#   a shots dir is given.
# Needs: gamescope (+ gamescopectl), python3-xlib.

import glob
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RA = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else ROOT + "/retroarch"
SHOTS = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
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


def main():
    if not os.access(RA, os.X_OK):
        print("No RetroArch binary at %s" % RA)
        return 1
    work = tempfile.mkdtemp(prefix="url_e2e_catalog.")
    home = work + "/home"
    for d in ("home", "assets", "saves", "system", "cache"):
        os.makedirs(work + "/" + d)
    cmd_port = free_port()
    web_port = free_port()
    base = "http://127.0.0.1:%d" % web_port
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
history_list_enable = "false"
core_assets_directory = "%s/assets"
savefile_directory = "%s/saves"
savestate_directory = "%s/saves"
system_directory = "%s/system"
cache_directory = "%s/cache"
""" % (cmd_port, work, work, work, work, work))

    procs = []
    fails = [0]

    def check(ok, what):
        print(("PASS: " if ok else "FAIL: ") + what)
        if not ok:
            fails[0] += 1

    try:
        web = subprocess.Popen(NICE + ["python3", SERVE, str(web_port)],
                               stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
        procs.append(web)
        if not wait_port(web_port):
            print("FAIL: serve.py did not start")
            return 1

        env = dict(os.environ, HOME=home, XDG_CONFIG_HOME=home + "/.config",
                   ASAN_OPTIONS="log_path=%s/asan:exitcode=0" % work)
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
        d = None
        for _ in range(50):
            try:
                d = display.Display(gdisp)
                break
            except Exception:
                time.sleep(0.2)
        if d is None:
            print("FAIL: cannot open gamescope's display %s" % gdisp)
            return 1
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

        def type_line(text):
            for c in text:
                key(c)
            key("Return")
            time.sleep(1.5)

        def cmd(c, wait=0.8):
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            for _ in range(8):
                s.sendto(c.encode(), ("127.0.0.1", cmd_port))
                time.sleep(0.002)
            time.sleep(wait)

        gs_wl = genv.get("GAMESCOPE_WAYLAND_DISPLAY")

        def snap(name):
            # gamescope's own screenshot of its (headless) screen, asked
            # through this gamescope's control socket only.
            if not SHOTS or not gs_wl:
                return
            os.makedirs(SHOTS, exist_ok=True)
            out = "%s/%s.png" % (SHOTS, name)
            if os.path.exists(out):
                os.remove(out)
            subprocess.run(["gamescopectl", "screenshot", out],
                           env=dict(os.environ, GAMESCOPE_WAYLAND_DISPLAY=gs_wl,
                                    XDG_RUNTIME_DIR=genv.get(
                                        "XDG_RUNTIME_DIR", "")),
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=10)
            for _ in range(30):
                if os.path.exists(out) and os.path.getsize(out) > 0:
                    time.sleep(0.3)
                    print("shot: %s" % out)
                    return
                time.sleep(0.1)
            print("note: no screenshot for %s" % name)

        def log():
            return open(work + "/ra.log", errors="replace").read()

        def servers():
            p = work + "/url_servers.cfg"
            return open(p).read() if os.path.exists(p) else None

        def authlog():
            return urllib.request.urlopen(base + "/authlog").read().decode()

        def cached(name):
            return [f for f in glob.glob(work + "/assets/url_cache/*")
                    if f.endswith("_" + name)]

        # Main Menu > Load Content > Load from URL, a folder link.
        cmd("MENU_DOWN")
        cmd("MENU_B")
        snap("catalog-0-load-content")
        cmd("MENU_B")
        type_line(base + "/cat/")
        time.sleep(1.5)
        check("Game list with 2 item(s)" in log(),
              "folder link opened a game list (2 items)")
        s = servers()
        check(s is not None and (base + "/") in s,
              "server saved to url_servers.cfg (%r)" % s)
        check(not glob.glob(work + "/assets/url_cache/*"),
              "nothing cached for the list")
        snap("catalog-1-root")

        # First row is the folder: open it.
        cmd("MENU_B", 2.5)
        check("Game list with 1 item(s)" in log(),
              "[Game Boy] opened one level deeper")
        snap("catalog-2-game-boy")

        # Back to /cat/, second row is x.gb: download it.
        cmd("MENU_A")
        snap("catalog-3-back")
        cmd("MENU_DOWN")
        cmd("MENU_B", 3)
        check(len(cached("x.gb")) == 1, "x.gb from the list downloaded")
        snap("catalog-4-file-picked")

        # Back out to Load Content; Saved Servers is there now.
        for _ in range(4):
            cmd("MENU_A", 0.5)
        cmd("MENU_DOWN")
        cmd("MENU_B")
        snap("catalog-5-load-content-again")

        # Empty folder by link (the cursor is on Load from URL).
        cmd("MENU_B")
        type_line(base + "/cat/empty/")
        time.sleep(1.5)
        check("Game list with 0 item(s)" in log(), "empty folder opened")
        snap("catalog-6-empty")
        cmd("MENU_A")

        # Over 4096 items: the first 4096 and a notification.
        cmd("MENU_B")
        type_line(base + "/cat/big/")
        time.sleep(2)
        check("Game list with 4096 item(s)" in log(),
              "big folder cut to 4096 items")
        snap("catalog-6b-truncated")
        cmd("MENU_A")

        # Saved Servers: second row of Load Content.
        cmd("MENU_DOWN")
        cmd("MENU_B")
        snap("catalog-7-saved-servers")
        n_before = log().count("Game list with")
        cmd("MENU_B", 3)
        check(log().count("Game list with") == n_before + 1,
              "saved server opened its game list")
        snap("catalog-8-server-root")
        cmd("MENU_A")
        # Row 2: "Remove this server".
        cmd("MENU_DOWN")
        cmd("MENU_B", 1.5)
        s = servers()
        check(s is not None and (base + "/") not in s,
              "server removed from url_servers.cfg (%r)" % s)
        snap("catalog-9-removed")
        cmd("MENU_A")

        # A list behind a login: typed once, saved, reused deeper.
        # Saved Servers is gone, the cursor is on the row after Load
        # from URL.
        authlog()   # reset
        cmd("MENU_UP")
        snap("catalog-10-no-saved-servers")
        cmd("MENU_B")
        type_line(base + "/basic/cat/")
        time.sleep(1.5)
        snap("catalog-11-login-prompt")
        type_line("bob")
        type_line("pw")
        time.sleep(1.5)
        logins = work + "/url_logins.cfg"
        check(os.path.exists(logins) and "bob" in open(logins).read(),
              "login typed for a list saved to url_logins.cfg")
        snap("catalog-12-login-list")
        cmd("MENU_B", 3)
        al = authlog()
        # A second prompt would block here and the list would not open.
        check(log().count("Game list with 1 item(s)") >= 2
              and "Basic" in al,
              "deeper folder used the saved login, no prompt (auth: %s)"
              % al)
        snap("catalog-13-login-deeper")

        cmd("QUIT")
        rc = None
        for _ in range(200):
            if os.path.exists(work + "/rc"):
                try:
                    rc = int(open(work + "/rc").read().strip() or "-1")
                    break
                except ValueError:
                    pass
            time.sleep(0.1)
        check(rc == 0, "RetroArch exited with 0 (%s)" % rc)
        # An ASan build (make SANITIZER=address) also has its report
        # checked: no memory errors at all, no leak from the URL code
        # (upstream RetroArch and Qt leak at exit; exitcode=0 keeps
        # those from failing the exit check, the report is read here).
        asan = "".join(open(f, errors="replace").read()
                       for f in glob.glob(work + "/asan*"))
        errors = [l for l in asan.splitlines()
                  if "ERROR: AddressSanitizer" in l]
        url_leaks = [b for b in asan.split("\n\n")
                     if "leak of" in b and ("url_" in b or "task_url" in b)]
        check(not errors, "no AddressSanitizer error %s" % errors[:1])
        check(not url_leaks, "no leak from the URL code%s"
              % ("" if not url_leaks else ":\n" + url_leaks[0][:2000]))
    finally:
        for p in reversed(procs):
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
    if fails[0]:
        print("url_e2e_catalog: %d check(s) failed, work dir kept: %s"
              % (fails[0], work))
        return 1
    print("url_e2e_catalog: all checks passed")
    shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
