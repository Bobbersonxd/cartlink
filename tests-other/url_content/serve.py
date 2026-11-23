#!/usr/bin/env python3
# Copyright (C) 2026 Cartlink contributors. GNU GPL v3 or later (see COPYING).
# Local test server for test_url_fetch.c. Silent, loopback only:
# listens on 127.0.0.1 and 127.0.0.2 (a "different host" for the
# cross-host redirect test). With a second port and a certificate, the
# same routes are also served over https on 127.0.0.1.
# Usage: serve.py <port> [<https port> <cert.pem> <key.pem>]

import hashlib
import http.server
import socketserver
import ssl
import sys
import threading
import time

GAME = bytes((i * 7 + 3) & 0xFF for i in range(8192))
PAGE = b"<!DOCTYPE html>\n<html><body>Sign in</body></html>\n"
USER = "testuser"
PASS = "pw"
REALM = "retro"
NONCE = "dcd98b7102dd2f0e8b11d0f600bfb0c093"
BIG = 300 * 1024 * 1024
HUGE = 3 * 1024 * 1024 * 1024
LOGIN_POSTS = 0   # POSTs that reached /xauthn/?op=login
AUTH_LOG = []     # Authorization scheme of each /basic/ and /digest/ GET
SLOW = 16 * 1024 * 1024
LAST_ACCEPT = ""  # Accept header of the latest GET
HTTP_PORT = 0     # both set in main(), for the http <-> https redirects
HTTPS_PORT = 0
HTTPS_PORT2 = 0   # a second https port (OS-picked), same routes
GBDUMMY = b"GBDUMMY"


def _zip(members):
    import io
    import zipfile
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in members:
            z.writestr(zipfile.ZipInfo(name), data)
    return buf.getvalue()


# Program-looking bytes (dummy, never a real program).
_PE = bytearray(1024)
_PE[0:2] = b"MZ"
_PE[0x3C] = 0x80
_PE[0x80:0x84] = b"PE\0\0"
PE = bytes(_PE)
ELF = b"\x7fELF\x02\x01\x01" + bytes(57)
_DOS = bytearray(512)
_DOS[0:2] = b"MZ"
_DOS[0x3C:0x40] = b"\x34\x00\x12\x00"   # e_lfanew past the end: no PE
DOSGAME_ZIP = _zip([("GAME.EXE", bytes(_DOS)),
                    ("RUN.BAT", b"@echo off\r\nGAME.EXE\r\n"),
                    ("DATA.DAT", GAME)])
SLIP_ZIP = _zip([("../evil.gb", GAME)])
CHUNKED_BIG = 20 * 1024 * 1024


def caddy_item(name, is_dir, url, size):
    # One entry the way Caddy's file_server browse sends it as JSON
    # (folder names end in "/").
    return {"name": name, "size": size, "url": url,
            "mod_time": "2026-11-01T12:00:00Z",
            "mode": 2147484141 if is_dir else 420,
            "is_dir": is_dir, "is_symlink": False}


# Listings under /cat/ (path after the prefix, decoded).
CATALOG = {
    "/cat/": [caddy_item("x.gb", False, "./x.gb", 7),
              caddy_item("Game Boy/", True, "./Game%20Boy/", 4096)],
    "/cat/Game Boy/": [caddy_item("Space & Time #2.gb", False,
                                  "./Space%20&%20Time%20%232.gb", 7)],
    "/cat/empty/": [],
    "/": [caddy_item("cat/", True, "./cat/", 4096)],
    # More than the 4096 items a game list shows.
    "/cat/big/": [caddy_item("g%04d.gb" % i, False, "./g%04d.gb" % i, 7)
                  for i in range(4100)],
}
CATALOG_FILES = {"/cat/x.gb", "/cat/Game Boy/Space & Time #2.gb"}


def md5(s):
    return hashlib.md5(s.encode("utf-8")).hexdigest()


def parse_digest(value):
    out = {}
    rest = value[len("Digest "):]
    while rest:
        rest = rest.lstrip(" ,")
        if "=" not in rest:
            break
        key, rest = rest.split("=", 1)
        key = key.strip()
        if rest.startswith('"'):
            end = rest.find('"', 1)
            if end < 0:
                break
            out[key] = rest[1:end]
            rest = rest[end + 1:]
        else:
            end = rest.find(",")
            if end < 0:
                end = len(rest)
            out[key] = rest[:end].strip()
            rest = rest[end:]
    return out


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def log_error(self, fmt, *args):
        pass

    def send_body(self, code, body, ctype="application/octet-stream",
                  extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        for k, v in (extra or []):
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def basic_ok(self):
        import base64
        want = "Basic " + base64.b64encode(
            (USER + ":" + PASS).encode()).decode()
        return self.headers.get("Authorization", "") == want

    def digest_ok(self):
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Digest "):
            return False
        p = parse_digest(auth)
        if p.get("username") != USER or p.get("realm") != REALM:
            return False
        if p.get("nonce") != NONCE or p.get("uri") != self.path:
            return False
        ha1 = md5(USER + ":" + REALM + ":" + PASS)
        ha2 = md5("GET:" + p["uri"])
        if p.get("qop") == "auth":
            want = md5(":".join([ha1, NONCE, p.get("nc", ""),
                                 p.get("cnonce", ""), "auth", ha2]))
        else:
            want = md5(ha1 + ":" + NONCE + ":" + ha2)
        return p.get("response") == want

    def redirect(self, location):
        self.send_response(302)
        self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()

    # Fake archive.org xauthn: POST /xauthn/?op=login with a form body.
    def do_POST(self):
        import json
        import urllib.parse
        n = int(self.headers.get("Content-Length", "0") or "0")
        body = self.rfile.read(n).decode("utf-8", "replace")
        if self.path == "/xauthn/redirect?op=login":
            self.redirect("/xauthn/?op=login")
            return
        if self.path != "/xauthn/?op=login":
            self.send_body(404, b"not found", "text/plain")
            return
        global LOGIN_POSTS
        LOGIN_POSTS += 1
        if self.headers.get("Content-Type") != \
                "application/x-www-form-urlencoded":
            self.send_body(400, b"bad content type", "text/plain")
            return
        form = urllib.parse.parse_qs(body, keep_blank_values=True)
        email = form.get("email", [""])[0]
        password = form.get("password", [""])[0]
        if email == "user@example.com" and password == "p&ss w=rd":
            reply = {"success": True, "values": {
                "email": email, "screenname": "testuser",
                "s3": {"access": "ACCESSKEY", "secret": "SECRETKEY"}},
                "version": 1}
        elif email == "user@example.com":
            reply = {"success": False,
                     "values": {"reason": "account_bad_password"},
                     "version": 1}
        elif email == "page@example.com":
            self.send_body(200, PAGE, "text/html")
            return
        elif email == "user401@example.com":
            # archive.org's way: an error status with a JSON reason.
            self.send_body(401, json.dumps({
                "success": False,
                "values": {"reason": "account_bad_password"},
                "version": 1}).encode(), "application/json")
            return
        elif email == "html401@example.com":
            self.send_body(401, PAGE, "text/html")
            return
        else:
            reply = {"success": False,
                     "values": {"reason": "account_not_found"},
                     "version": 1}
        self.send_body(200, json.dumps(reply).encode(), "application/json")

    def log_auth(self):
        auth = self.headers.get("Authorization")
        AUTH_LOG.append(auth.split(" ", 1)[0] if auth else "none")

    # Caddy-style folder listing: JSON only when the client accepts it,
    # else an HTML page. True if path was a catalog route.
    def catalog(self, path):
        import json
        import urllib.parse
        p = urllib.parse.unquote(path)
        if p in CATALOG:
            if "application/json" in self.headers.get("Accept", ""):
                self.send_body(200, json.dumps(CATALOG[p]).encode(),
                               "application/json")
            else:
                self.send_body(200, PAGE, "text/html; charset=utf-8")
            return True
        if p in CATALOG_FILES:
            self.send_body(200, GBDUMMY)
            return True
        return False

    def do_GET(self):
        global AUTH_LOG, LAST_ACCEPT
        path = self.path
        port = self.server.server_address[1]
        LAST_ACCEPT = self.headers.get("Accept", "")
        if path.startswith("/acceptlog"):
            self.send_body(200, ("accept:" + LAST_ACCEPT).encode(),
                           "text/plain")
            return
        if path == "/plain.json":
            self.send_body(200, b'{"hi":1}', "application/json")
            return
        if (path.startswith("/cat/") or path == "/") and self.catalog(path):
            return
        if path.startswith("/basic/cat/"):
            self.log_auth()
            if not self.basic_ok():
                self.send_body(401, b"", "text/plain", [
                    ("WWW-Authenticate", 'Basic realm="' + REALM + '"')])
            elif not self.catalog(path[len("/basic"):]):
                self.send_body(404, b"not found", "text/plain")
            return
        if path.startswith("/xauthn/count"):
            self.send_body(200, ("%d" % LOGIN_POSTS).encode(), "text/plain")
            return
        if path.startswith("/authlog"):
            body = (",".join(AUTH_LOG) or "empty").encode()
            AUTH_LOG = []
            self.send_body(200, body, "text/plain")
            return
        if path.startswith("/basic/") or path.startswith("/digest/"):
            self.log_auth()
        if path == "/basicecho":
            # Asks for Basic, then echoes what it got.
            auth = self.headers.get("Authorization")
            if auth:
                self.send_body(200, ("auth:" + auth).encode(), "text/plain")
            else:
                self.send_body(401, b"", "text/plain", [
                    ("WWW-Authenticate", 'Basic realm="' + REALM + '"')])
        elif path == "/echoauth":
            auth = self.headers.get("Authorization")
            body = ("auth:" + auth).encode() if auth else b"none"
            self.send_body(200, body, "text/plain")
        elif path == "/xhost":
            self.redirect("http://127.0.0.2:%d/echoauth" % port)
        elif path == "/xhost-s":
            # Only meant for the https side: to a name the test
            # certificate does not cover.
            self.redirect("https://127.0.0.1:%d/game.sfc" % port)
        elif path == "/tohttps":
            # http -> https on localhost (the name on the certificate).
            self.redirect("https://localhost:%d/basic/game.sfc" % HTTPS_PORT)
        elif path == "/toport":
            # https -> the same host on another https port.
            self.redirect("https://localhost:%d/basic/game.sfc" % HTTPS_PORT2)
        elif path == "/tohttp":
            # https -> plain http page that asks for a login.
            self.redirect("http://localhost:%d/basic/game.sfc" % HTTP_PORT)
        elif path == "/samehost":
            self.redirect("/basic/game.sfc")
        elif path == "/loop":
            self.redirect("/loop")
        elif path.startswith("/chain/"):
            n = int(path[len("/chain/"):])
            self.redirect("/game.sfc" if n <= 1 else "/chain/%d" % (n - 1))
        elif path == "/rel/a":
            self.redirect("b")
        elif path == "/rel/b":
            self.send_body(200, GAME)
        elif path == "/empty":
            self.send_body(200, b"")
        elif path == "/huge":
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(HUGE))
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                self.wfile.write(bytes(1024))
            except (BrokenPipeError, ConnectionResetError):
                pass
            self.close_connection = True
        elif path == "/game.sfc":
            self.send_body(200, GAME)
        elif path == "/prog.exe":
            self.send_body(200, PE)
        elif path == "/sneaky.gb":
            self.send_body(200, ELF)
        elif path == "/named-prog":
            # A harmless-looking link whose server names it .exe.
            self.send_body(200, GAME, extra=[
                ("Content-Disposition", 'attachment; filename="setup.exe"')])
        elif path == "/dosgame.zip":
            self.send_body(200, DOSGAME_ZIP)
        elif path == "/slip.zip":
            self.send_body(200, SLIP_ZIP)
        elif path == "/slip.dat":
            # The same bytes under a name that is not an archive.
            self.send_body(200, SLIP_ZIP)
        elif path == "/chunked" or path == "/chunkedbig":
            # Transfer-Encoding: chunked, in uneven pieces.
            body = GAME if path == "/chunked" else \
                bytes((i * 7 + 3) & 0xFF for i in range(256)) * \
                (CHUNKED_BIG // 256)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Connection", "close")
            self.end_headers()
            pos = 0
            step = 1
            try:
                while pos < len(body):
                    n = min(len(body) - pos, step)
                    self.wfile.write(b"%x\r\n" % n + body[pos:pos + n]
                                     + b"\r\n")
                    pos += n
                    step = step * 3 + 1 if step < 300000 else 1
                self.wfile.write(b"0\r\n\r\n")
            except (BrokenPipeError, ConnectionResetError):
                pass
            self.close_connection = True
        elif path == "/nodisp?id=1":
            self.send_body(200, GAME, extra=[
                ("Content-Disposition", 'attachment; filename="Named.gba"')])
        elif path == "/page":
            self.send_body(200, PAGE, "text/html")
        elif path == "/basic/game.sfc":
            if self.basic_ok():
                self.send_body(200, GAME)
            else:
                self.send_body(401, b"", "text/plain", [
                    ("WWW-Authenticate", 'Basic realm="' + REALM + '"')])
        elif path == "/digest/game.sfc":
            if self.digest_ok():
                self.send_body(200, GAME)
            else:
                self.send_body(401, b"", "text/plain", [
                    ("WWW-Authenticate",
                     'Digest realm="' + REALM + '", qop="auth", nonce="'
                     + NONCE + '", opaque="5ccc069c403ebaf9f0171e9517f40e41"')])
        elif path == "/redirect":
            self.redirect("/game.sfc")
        elif path == "/drop":
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", "1000000")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(GAME[:1000])
            self.wfile.flush()
            self.close_connection = True
        elif path == "/slowsmall":
            # 2 MiB in about 1 s (two downloads of it overlap).
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(2 * 1024 * 1024))
            self.send_header("Connection", "close")
            self.end_headers()
            chunk = bytes((i * 7 + 3) & 0xFF for i in range(64 * 1024))
            try:
                for _ in range(32):
                    self.wfile.write(chunk)
                    self.wfile.flush()
                    time.sleep(0.03)
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
        elif path == "/slow":
            # 16 MiB in about 10 s: still running when a test quits, and
            # the progress percentage changes every ~0.1 s.
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(SLOW))
            self.send_header("Connection", "close")
            self.end_headers()
            chunk = bytes(64 * 1024)
            left = SLOW
            try:
                while left > 0:
                    self.wfile.write(chunk)
                    self.wfile.flush()
                    left -= len(chunk)
                    time.sleep(0.04)
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
        elif path == "/big":
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(BIG))
            self.send_header("Connection", "close")
            self.end_headers()
            chunk = bytes(1024 * 1024)
            left = BIG
            try:
                while left > 0:
                    n = min(left, len(chunk))
                    self.wfile.write(chunk[:n])
                    left -= n
            except (BrokenPipeError, ConnectionResetError):
                pass
        else:
            self.send_body(404, b"not found", "text/plain")


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def handle_error(self, request, client_address):
        pass


def main():
    global HTTP_PORT, HTTPS_PORT
    port = int(sys.argv[1])
    HTTP_PORT = port
    if len(sys.argv) >= 3:
        HTTPS_PORT = int(sys.argv[2])
    other = Server(("127.0.0.2", port), Handler)
    threading.Thread(target=other.serve_forever, daemon=True).start()
    if len(sys.argv) >= 5:
        tls = Server(("127.0.0.1", int(sys.argv[2])), Handler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(sys.argv[3], sys.argv[4])
        tls.socket = ctx.wrap_socket(tls.socket, server_side=True)
        threading.Thread(target=tls.serve_forever, daemon=True).start()
        global HTTPS_PORT2
        tls2 = Server(("127.0.0.1", 0), Handler)
        tls2.socket = ctx.wrap_socket(tls2.socket, server_side=True)
        HTTPS_PORT2 = tls2.server_address[1]
        threading.Thread(target=tls2.serve_forever, daemon=True).start()
    Server(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
