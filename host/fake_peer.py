#!/usr/bin/env python3
# Author: trashman (VitaSend)
"""
fake_peer.py - pretends to be a LocalSend desktop app so the protocol code can be tested on a PC.

It starts the PC build of this project (host/localsend-host), then plays the role of another device:
  * runs an HTTPS server (self-signed RSA cert, like the real apps) that must receive /register,
  * sends/receives the UDP "announce" messages,
  * calls /info and /register on our server like a real client would,
  * throws some malformed requests at it.

    python3 host/fake_peer.py                      # run everything
    python3 host/fake_peer.py --chunked-replies    # make the fake server answer with chunked encoding (like Dart does)

Needs: python3 and the openssl command line tool.   Exit code 0 = all checks passed.
"""
import argparse, hashlib, http.client, json, os, socket, ssl, subprocess, sys, tempfile, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument("--binary", default=os.path.join(HERE, "localsend-host"))
ap.add_argument("--vita-port", type=int, default=53320)
ap.add_argument("--peer-port", type=int, default=53399)
ap.add_argument("--chunked-replies", action="store_true")
ap.add_argument("--keep", action="store_true", help="leave the data dir and screen dump in place")
args = ap.parse_args()

VITA = "127.0.0.1"
tmp = tempfile.mkdtemp(prefix="ls-fake-")
screen_file = os.path.join(tmp, "screen.txt")
data_dir = os.path.join(tmp, "data")

results = []
def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(("  PASS  " if ok else "  FAIL  ") + name + (("  -> " + str(detail)) if (detail and not ok) else ""))
    return ok

# ---------------------------------------------------------------- fake peer certificate (RSA, like the desktop app)
key_pem, crt_pem = os.path.join(tmp, "peer.key"), os.path.join(tmp, "peer.crt")
subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key_pem, "-out", crt_pem,
                "-days", "30", "-subj", "/CN=Fake Desktop"], check=True, stderr=subprocess.DEVNULL)
der = ssl.PEM_cert_to_DER_cert(open(crt_pem).read())
PEER_FP = hashlib.sha256(der).hexdigest()
PEER_INFO = {"alias": "Secret Banana", "version": "2.1", "deviceModel": "Windows", "deviceType": "desktop",
             "fingerprint": PEER_FP, "port": args.peer_port, "protocol": "https", "download": False}

# ---------------------------------------------------------------- the fake peer's HTTPS server
registers = []          # bodies of POST /register received from the Vita
reg_event = threading.Event()

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def _reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Connection", "close")
        if args.chunked_replies:
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            half = len(body) // 2
            for part in (body[:half], body[half:]):
                self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        self.close_connection = True
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(n)
        if self.path == "/api/localsend/v2/register":
            try:
                registers.append(json.loads(body))
            except Exception:
                registers.append({"_bad": body.decode(errors="replace")})
            reg_event.set()
            self._reply(200, PEER_INFO)
        else:
            self._reply(404, {"message": "nope"})
    def do_GET(self):
        if self.path.startswith("/api/localsend/v2/info"):
            self._reply(200, {k: v for k, v in PEER_INFO.items() if k not in ("port", "protocol")})
        else:
            self._reply(404, {"message": "nope"})

srv = ThreadingHTTPServer(("127.0.0.1", args.peer_port), Handler)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(crt_pem, key_pem)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
threading.Thread(target=srv.serve_forever, daemon=True).start()

# ---------------------------------------------------------------- UDP side
udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
udp.bind(("127.0.0.1", args.peer_port))
udp.settimeout(0.2)
udp_msgs = []
def udp_loop():
    while True:
        try:
            data, addr = udp.recvfrom(4096)
            udp_msgs.append(json.loads(data))
        except socket.timeout:
            continue
        except Exception:
            return
threading.Thread(target=udp_loop, daemon=True).start()

def send_udp(obj):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(json.dumps(obj).encode(), (VITA, args.vita_port))
    s.close()

def wait_for(pred, timeout=6.0):
    end = time.time() + timeout
    while time.time() < end:
        if pred(): return True
        time.sleep(0.05)
    return False

# ---------------------------------------------------------------- HTTPS client helpers
cctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
cctx.check_hostname = False
cctx.verify_mode = ssl.CERT_NONE

def https(method, path, body=None, headers=None):
    c = http.client.HTTPSConnection(VITA, args.vita_port, context=cctx, timeout=8)
    h = {"Connection": "close"}
    if body is not None:
        h["Content-Type"] = "application/json"
    h.update(headers or {})
    c.request(method, path, body=body, headers=h)
    sock = c.sock                       # grab TLS details before the response closes the connection
    info = (sock.version(), sock.cipher()[0], sock.getpeercert(binary_form=True))
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, data, info

def raw_tls(payload: bytes, wrap=True) -> bytes:
    s = socket.create_connection((VITA, args.vita_port), timeout=8)
    if wrap: s = cctx.wrap_socket(s)
    s.sendall(payload)
    out = b""
    try:
        while True:
            d = s.recv(4096)
            if not d: break
            out += d
    except Exception:
        pass
    s.close()
    return out

# ---------------------------------------------------------------- start the program under test
_probe = socket.socket()
_probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)     # ignore TIME_WAIT leftovers
try:
    _probe.bind(("0.0.0.0", args.vita_port))
except OSError:
    sys.exit(f"port {args.vita_port} is already in use - is an old localsend-host still running? (pkill -x localsend-host)")
_probe.close()
env = dict(os.environ)
log_path = os.path.join(tmp, "vita.log")
proc = subprocess.Popen([args.binary, "--data", data_dir, "--port", str(args.vita_port),
                         "--announce-to", f"127.0.0.1:{args.peer_port}", "--seconds", "120",
                         "--dump-screen", screen_file, "--keys", "wait1500,r"],      # r = the Send tab, where devices are listed
                        stdout=open(log_path, "w"), stderr=subprocess.STDOUT, env=env)

def finish(code):
    proc.terminate()
    try: proc.wait(5)
    except Exception: proc.kill()
    if args.keep:
        print("kept:", tmp)
    sys.exit(code)

import atexit
atexit.register(lambda: proc.poll() is None and proc.kill())

print("== waiting for the program to come up")
up = wait_for(lambda: udp_msgs, 10)
if not check("Vita announces itself over UDP at startup", up):
    print(open(log_path).read()); finish(1)

# ---------------------------------------------------------------- 1. what the announcement looks like
print("== announcement format")
a = udp_msgs[0]
check("announcement has alias/version/deviceModel/deviceType/fingerprint/port/protocol",
      all(k in a for k in ("alias", "version", "deviceModel", "deviceType", "fingerprint", "port", "protocol")), a)
check("announce is true (and legacy announcement too)", a.get("announce") is True and a.get("announcement") is True, a)
check("fingerprint is 64 lowercase hex chars", len(a["fingerprint"]) == 64 and all(c in "0123456789abcdef" for c in a["fingerprint"]), a["fingerprint"])
check("protocol is https and port matches", a["protocol"] == "https" and a["port"] == args.vita_port, a)
check("deviceType is a valid enum value", a["deviceType"] in ("mobile", "desktop", "web", "headless", "server"), a["deviceType"])
VITA_FP = a["fingerprint"]
check("startup burst sent several announcements", wait_for(lambda: len(udp_msgs) >= 4, 6), len(udp_msgs))

# ---------------------------------------------------------------- 2. GET /info over TLS
print("== GET /info")
st, body, (ver, cipher, cert) = https("GET", "/api/localsend/v2/info")
j = json.loads(body)
check("/info -> 200 JSON", st == 200 and isinstance(j, dict), (st, body))
check("TLS 1.2 negotiated", ver == "TLSv1.2", (ver, cipher))
check("/info fingerprint == SHA-256 of the certificate presented in the TLS handshake",
      j.get("fingerprint") == hashlib.sha256(cert).hexdigest(), (j.get("fingerprint"), hashlib.sha256(cert).hexdigest()))
check("/info fingerprint == announced fingerprint", j.get("fingerprint") == VITA_FP)
check("/info has version 2.x and download flag", j.get("version", "").startswith("2.") and j.get("download") is False, j)
st, body, _ = https("GET", f"/api/localsend/v2/info?fingerprint={VITA_FP}")
check("/info?fingerprint=<own> -> 412 (self-discovery)", st == 412, (st, body))

# ---------------------------------------------------------------- 3. peer announces -> Vita must POST /register to us
print("== multicast announce -> HTTPS register")
registers.clear(); reg_event.clear()
send_udp(dict(PEER_INFO, announce=True))
got = reg_event.wait(6)
check("Vita sent POST /api/localsend/v2/register to the announcing peer's HTTPS server", got)
if got:
    r = registers[0]
    check("register body identifies the Vita (alias, fingerprint, port, protocol)",
          r.get("fingerprint") == VITA_FP and r.get("alias") == a["alias"] and r.get("port") == args.vita_port
          and r.get("protocol") == "https" and r.get("deviceType") == a["deviceType"], r)
check("peer shows up in the Vita's peer list (screen)", wait_for(lambda: os.path.exists(screen_file) and "Secret Banana" in open(screen_file).read(), 4))

# ---------------------------------------------------------------- 4. desktop registers with the Vita
print("== POST /register from a desktop")
other = dict(PEER_INFO, alias="Cute Orange", fingerprint="b" * 64)
other.pop("download")
st, body, _ = https("POST", "/api/localsend/v2/register", json.dumps(other))
j = json.loads(body) if st == 200 else {}
check("/register -> 200 with the Vita's own info", st == 200 and j.get("fingerprint") == VITA_FP and j.get("alias") == a["alias"], (st, body))
check("/register reply includes port + protocol", j.get("port") == args.vita_port and j.get("protocol") == "https", j)
check("registered device appears on screen", wait_for(lambda: "Cute Orange" in open(screen_file).read(), 4))

body_js = json.dumps(dict(other, alias="Chunky Mango", fingerprint="c" * 64)).encode()
chunks = b"".join(b"%x\r\n" % len(p) + p + b"\r\n" for p in (body_js[:20], body_js[20:]))
out = raw_tls(b"POST /api/localsend/v2/register HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
              b"Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n" + chunks + b"0\r\n\r\n")
check("chunked request body accepted (Dart's default for JSON)", out.startswith(b"HTTP/1.1 200"), out[:80])
check("chunked-registered device appears on screen", wait_for(lambda: "Chunky Mango" in open(screen_file).read(), 4))

st, body, _ = https("POST", "/api/localsend/v2/register", json.dumps(dict(other, fingerprint=VITA_FP)))
check("/register with the Vita's own fingerprint -> 412", st == 412, (st, body))
st, body, _ = https("POST", "/api/localsend/v2/register", "{not json")
check("/register with broken JSON -> 400", st == 400, (st, body))
st, body, _ = https("POST", "/api/localsend/v2/register", json.dumps({"alias": "x"}))
check("/register without fingerprint -> 400", st == 400, (st, body))
st, body, _ = https("POST", "/api/localsend/v2/register", json.dumps({"alias": "Émoji 😀 ä", "fingerprint": "d" * 64}))
check("/register with non-ASCII alias -> 200", st == 200, (st, body))

# ---------------------------------------------------------------- 5. routing / errors
print("== routing and error handling")
st, body, _ = https("GET", "/nothing")
check("unknown path -> 404 JSON", st == 404 and json.loads(body).get("message"), (st, body))
st, body, _ = https("POST", "/api/localsend/v2/info", "{}")
check("POST /info -> 405", st == 405, st)
st, body, _ = https("POST", "/api/localsend/v2/prepare-upload", "{}")
check("prepare-upload with an empty body -> 400 (the real tests are in test_transfer.py)", st == 400, st)
out = raw_tls(b"GET /api/localsend/v2/info HTTP/1.1\r\nHost: x\r\nX-Big: " + b"A" * 20000 + b"\r\n\r\n")
check("oversized header -> 431", out.startswith(b"HTTP/1.1 431"), out[:40])
out = raw_tls(b"GET / HTTP/1.1\r\n\r\n", wrap=False)
check("plain HTTP sent to the HTTPS port is dropped without a crash", True)
out = raw_tls(b"\x16\x03\x01garbage garbage garbage", wrap=False)
check("garbage bytes are dropped without a crash", True)

results_before = len(results)
outs = []
def hit():
    try: outs.append(https("GET", "/api/localsend/v2/info")[0])
    except Exception as e: outs.append(str(e))
ts = [threading.Thread(target=hit) for _ in range(5)]
[t.start() for t in ts]; [t.join() for t in ts]
check("5 parallel HTTPS requests all succeed", outs.count(200) == 5, outs)

# ---------------------------------------------------------------- 6. other announce variants
print("== announce variants")
registers.clear(); reg_event.clear()
send_udp(dict(PEER_INFO, alias="Quiet Peach", fingerprint="e" * 64, announce=False))
check("announce:false -> peer is listed", wait_for(lambda: "Quiet Peach" in open(screen_file).read(), 4))
check("announce:false -> Vita does NOT register back", not reg_event.wait(1.5))

send_udp(dict(PEER_INFO, fingerprint=VITA_FP, alias="Mirror Mirror", announce=True))
check("own announcement (same fingerprint) is ignored", not reg_event.wait(1.5) and "Mirror Mirror" not in open(screen_file).read())

legacy = dict(PEER_INFO, alias="Old Timer", fingerprint="f" * 64); legacy.pop("announce", None)
legacy["announcement"] = True
send_udp(legacy)
check("legacy v1-style 'announcement': true also triggers register", reg_event.wait(6))

# fallback: peer announces a port where nothing listens -> HTTP register fails -> UDP reply with announce:false
udp_msgs.clear()
send_udp(dict(PEER_INFO, alias="Ghost", fingerprint="9" * 64, port=args.peer_port + 1, announce=True))
check("register fails -> falls back to a UDP reply with announce:false",
      wait_for(lambda: any(m.get("announce") is False and m.get("fingerprint") == VITA_FP for m in udp_msgs), 10), udp_msgs[-3:])

# Re-announce on demand is exercised by the on-device TRIANGLE button; nothing to do here.

# ---------------------------------------------------------------- 7. health
print("== health")
check("program still running (no crash)", proc.poll() is None)
log = open(log_path).read()
check("no sanitizer / crash messages in the output", "ERROR: AddressSanitizer" not in log and "runtime error" not in log, log[-400:])

print()
passed = sum(results)
print(f"{passed}/{len(results)} checks passed")
if passed != len(results):
    print("--- program output ---"); print(log[-2500:])
    print("--- screen ---"); print(open(screen_file).read() if os.path.exists(screen_file) else "(none)")
else:
    print("--- screen as the Vita would show it ---"); print(open(screen_file).read())
finish(0 if passed == len(results) else 1)
