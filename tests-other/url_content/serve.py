#!/usr/bin/env python3
# Local test server for test_url_fetch.c. Silent, loopback only:
# listens on 127.0.0.1 and 127.0.0.2 (a "different host" for the
# cross-host redirect test). Usage: serve.py <port>

import hashlib
import http.server
import socketserver
import sys
import threading

GAME = bytes((i * 7 + 3) & 0xFF for i in range(8192))
PAGE = b"<!DOCTYPE html>\n<html><body>Sign in</body></html>\n"
USER = "bob"
PASS = "pw"
REALM = "retro"
NONCE = "dcd98b7102dd2f0e8b11d0f600bfb0c093"
BIG = 300 * 1024 * 1024
HUGE = 3 * 1024 * 1024 * 1024


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

    def do_GET(self):
        path = self.path
        port = self.server.server_address[1]
        if path == "/echoauth":
            auth = self.headers.get("Authorization")
            body = ("auth:" + auth).encode() if auth else b"none"
            self.send_body(200, body, "text/plain")
        elif path == "/xhost":
            self.redirect("http://127.0.0.2:%d/echoauth" % port)
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
    port = int(sys.argv[1])
    other = Server(("127.0.0.2", port), Handler)
    threading.Thread(target=other.serve_forever, daemon=True).start()
    Server(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
