#!/usr/bin/env python3
# Author: trashman (VitaSend)
"""
test_transfer.py - end-to-end tests for receiving and sending files, text messages, PIN, favorites, settings.

It starts the PC build of this program (host/localsend-host) and plays "the other device":

  * as a SENDER  (HTTPS client)   -> exercises prepare-upload / upload / cancel on the Vita
  * as a RECEIVER (HTTPS server)  -> the Vita sends files / text to it
  * as the USER   (button presses through a named pipe, screen read from a text dump)

    python3 host/test_transfer.py               # everything
    python3 host/test_transfer.py --only recv   # only the receiving half
    python3 host/test_transfer.py --only send

Needs: python3 and the openssl command line tool.   Exit code 0 = all checks passed.
"""
import re, argparse, atexit, hashlib, http.client, json, os, random, shutil, socket, ssl, struct, subprocess, sys, tempfile, threading, time, zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs


def _local_ip():
    """This machine's outgoing address - the same one the program shows on screen (plat_net_local_ip)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9)); return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()
LOCAL_IP = _local_ip()

HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument("--binary", default=os.path.join(HERE, "localsend-host"))
ap.add_argument("--only", default="", choices=["", "recv", "send", "extra", "install", "privacy", "theme", "touch", "migrate", "files", "exists", "retry", "queue", "polish", "dpad", "x11", "x13", "move", "wifi", "view", "update", "fileops", "shots"])
ap.add_argument("--shots-dir", default=os.path.join(HERE, "..", "docs", "screens"), help="with --only shots: where the pictures go")
ap.add_argument("--keep", action="store_true")
args = ap.parse_args()

ROOT = tempfile.mkdtemp(prefix="ls-xfer-")
results = []
def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(("  PASS  " if ok else "  FAIL  ") + name + (("  -> " + str(detail)[:300]) if (detail != "" and not ok) else ""))
    return ok

def wait_for(pred, timeout=8.0, step=0.05):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if pred(): return True
        except Exception:
            pass
        time.sleep(step)
    return False

def rnd(n, seed=1):
    return random.Random(seed).randbytes(n)

def sha(b):
    return hashlib.sha256(b).hexdigest()

# =========================================================================== the program under test
OSK_ROWS = ["1234567890-_=+", "qwertyuiop[]()", "asdfghjkl;:'\"/", "zxcvbnm,.?!@#$", "QWERTYUIOP{}<>", "ASDFGHJKL%^&*\\", "ZXCVBNM`~|    "]

VERSION = "0.9.7"
SET_ROWS = {"name": 0, "pin": 1, "hide": 2, "folder": 3, "accept": 4, "autoinst": 5, "exists": 6, "theme": 7, "battery": 8, "clock": 9, "saver": 10, "sound": 11, "about": 12, "checkupd": 13, "log": 14, "quit": 15}

def osk_keys(text, digits=False):
    """Button presses that type `text` on the on-screen keyboard (cursor starts at the top left)."""
    cols, rows = (10, 1) if digits else (14, 7)
    keys, r, c = [], 0, 0
    for ch in text:
        if ch == " " and not digits:
            keys.append("triangle"); continue
        if digits: tr, tc = 0, "1234567890".index(ch)
        else:
            tr, tc = next((i, row.index(ch)) for i, row in enumerate(OSK_ROWS) if ch in row[:14] and not (i == 6 and row.index(ch) >= 10))
        dc = (tc - c) % cols
        keys += ["right"] * dc if dc <= cols // 2 else ["left"] * (cols - dc)
        dr = (tr - r) % rows
        keys += ["down"] * dr if dr <= rows // 2 else ["up"] * (rows - dr)
        keys.append("cross")
        r, c = tr, tc
    return keys

OSK_PAGES = [OSK_ROWS,
    ["àáâãäåæçèéêëìí", "îïðñòóôõöøùúûü", "ýþżßœšžłąęćńśź", "ÀÁÂÃÄÅÆÇÈÉÊËÌÍ", "ÎÏÐÑÒÓÔÕÖØÙÚÛÜ", "ÝÞŻŒŠŽŁĄĘĆŃŚŹŸ", "ğışİĞŞ€£©°…—¿¡"],
    ["абвгдежзийклмн", "опрстуфхцчшщъы", "ьэюяёіїєґў’-.,", "АБВГДЕЖЗИЙКЛМН", "ОПРСТУФХЦЧШЩЪЫ", "ЬЭЮЯЁІЇЄҐЎ?!:;", "~\"'()@#$%&*+=/"],
    ["αβγδεζηθικλμνξ", "οπρσςτυφχψωάέή", "ίόύώϊϋΐΰ;·,.?!", "ΑΒΓΔΕΖΗΘΙΚΛΜΝΞ", "ΟΠΡΣΤΥΦΧΨΩΆΈΉΊ", "ΌΎΏΪΫ~\"'()@#$%", "&*/\\:;<>[]{}|`"]]

def osk_keys_unicode(text):
    """Like osk_keys, but also switches the keyboard pages (L / R) for accented letters, Cyrillic and Greek."""
    keys, page, r, c = [], 0, 0, 0
    for ch in text:
        if ch == " ":
            keys.append("triangle"); continue
        if ch == "\b":                                  # the delete key
            keys.append("square"); continue
        found = None
        for pg in range(4):
            for i, row in enumerate(OSK_PAGES[pg]):
                for j, k in enumerate(row):
                    if k == ch and not (pg == 0 and i == 6 and j >= 10):
                        found = (pg, i, j); break
                if found: break
            if found: break
        pg, tr, tc = found
        d = (pg - page) % 4
        keys += ["r"] * d if d <= 2 else ["l"] * (4 - d)
        page = pg
        dc = (tc - c) % 14
        keys += ["right"] * dc if dc <= 7 else ["left"] * (14 - dc)
        dr = (tr - r) % 7
        keys += ["down"] * dr if dr <= 3 else ["up"] * (7 - dr)
        keys.append("cross")
        r, c = tr, tc
    return keys

class Vita:
    def __init__(self, name, port, peer_port, config=None, browse=None, seconds=900, extra=None, env=None, ready=True):
        self.name, self.port = name, port
        self.dir = os.path.join(ROOT, name)
        self.data = os.path.join(self.dir, "data")
        os.makedirs(self.data)
        self.recv = os.path.join(self.dir, "received")
        cfg = {"receiveDir": self.recv}
        cfg.update(config or {})
        json.dump(cfg, open(os.path.join(self.data, "config.json"), "w"))
        self.screen_file = os.path.join(self.dir, "screen.txt")
        self.fifo = os.path.join(self.dir, "keys")
        self.log_path = os.path.join(self.dir, "out.log")
        cmd = [args.binary, "--data", self.data, "--port", str(port), "--announce-to", f"127.0.0.1:{peer_port}",
               "--seconds", str(seconds), "--dump-screen", self.screen_file, "--keys-fifo", self.fifo]
        if browse: cmd += ["--browse-start", browse]
        if extra: cmd += extra
        env = dict(dict(os.environ, LS_KEY_MS="60", ASAN_OPTIONS="detect_leaks=0", LS_UPDATE_API="localhost:1"), **(env or {}))   # (never the real GitHub: a dead port unless a test sets its own)
        self.proc = subprocess.Popen(cmd, stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT, env=env)
        ALL_VITAS.append(self)
        atexit.register(lambda: self.proc.poll() is None and self.proc.kill())
        assert wait_for(lambda: os.path.exists(self.fifo) and os.path.exists(self.screen_file), 10), "program did not start"
        self.kf = open(self.fifo, "w", buffering=1)
        self.fp = None
        if not ready: return                     # (Wi-Fi is off at the start: the Receive page only comes later)
        wait_for(lambda: "This device" in self.screen(), 10)
        self.fp = self.screen().split("Fingerprint")[1].split()[0]

    def screen(self):
        try: return open(self.screen_file).read()
        except Exception: return ""
    def has(self, text): return text in self.screen()
    def wait_screen(self, text, timeout=10):
        return wait_for(lambda: text in self.screen(), timeout)
    def keys(self, *names):
        self.kf.write(" ".join(names) + "\n")
    # ---- the front touch screen (simulated: "tap:X:Y" and "drag:X1:Y1:X2:Y2" in the key pipe) ----
    def tap(self, x, y): self.kf.write(f"tap:{x}:{y}\n")
    def drag(self, x1, y1, x2, y2): self.kf.write(f"drag:{x1}:{y1}:{x2}:{y2}\n")
    def hits(self):
        try: return [l.split() for l in open(self.screen_file + ".hits").read().splitlines() if l.strip()]
        except Exception: return []
    def find_hit(self, kind, val, hint=None):
        """The topmost touch area of this kind with this value; hint=True: only the hint bar, False: only above it."""
        for h in reversed(self.hits()):
            if h[0] != kind or int(h[5]) != val: continue
            on_bar = int(h[2]) >= 500
            if hint is None or hint == on_bar: return tuple(int(v) for v in h[1:5])
        return None
    def tap_hit(self, kind, val, hint=None):
        r = self.find_hit(kind, val, hint)
        if r: self.tap(r[0] + r[2] // 2, r[1] + r[3] // 2)
        return r is not None
    def menu_items(self):
        """The entries of the Files tab's options menu, as shown (background list rows are filtered out)."""
        lines = self.screen().splitlines()
        try: i = next(n for n, l in enumerate(lines) if l.strip().startswith("Options"))
        except StopIteration: return []
        out = []
        for l in lines[i + 1:]:
            t = l[1:].strip()
            if not t or t.startswith("[UP") or t.startswith("/") or re.match(r"\[[ x]\]\s", t): continue
            t = re.split(r"\s{2,}", t)[0]
            if t.startswith(("Send ", "Install", "Tick ", "Untick ", "Unzip ", "Rename ", "Delete ", "New folder here", "Open ", "Move ", "Copy ", "Cancel the move", "Cancel the copy")): out.append(t)
        return out
    def menu_index(self, prefix):
        return next(n for n, t in enumerate(self.menu_items()) if t.startswith(prefix))
    def menu_focused(self):
        """The text of the highlighted entry of the options menu ('' if none can be seen)."""
        lines = self.screen().splitlines()
        try: i = next(n for n, l in enumerate(lines) if l.strip().startswith("Options"))
        except StopIteration: return ""
        for l in lines[i + 1:]:
            if l.startswith(">"):
                t = re.split(r"\s{2,}", l[1:].strip())[0]
                if t.startswith(("Send ", "Install", "Tick ", "Untick ", "Unzip ", "Rename ", "Delete ", "New folder here", "Open ", "Move ", "Copy ", "Cancel the move", "Cancel the copy")): return t
        return ""
    def menu_pick(self, prefix):
        """Chooses an entry of the options menu with the buttons (the menu scrolls when it has more than 9 entries)."""
        self.keys(*(["up"] * 12))
        first = None
        def at_top():
            nonlocal first
            items = self.menu_items()
            first = items[0] if items else None
            return first is not None and self.menu_focused() == first
        if not wait_for(at_top, 8): raise AssertionError("menu cursor did not reach the top: " + self.screen())
        for _ in range(14):
            cur = self.menu_focused()
            if cur.startswith(prefix):
                self.keys("cross"); return
            self.keys("down")
            if not wait_for(lambda: self.menu_focused() not in ("", cur), 4): break
        raise AssertionError("menu entry not found: " + prefix + " in " + self.screen())
    def type_text(self, text, digits=False):
        self.keys(*osk_keys(text, digits))
    def goto_setting(self, name):
        """Highlights a row of the Settings tab (opening the tab with [L] if we are on the Receive tab)."""
        if "screen=settings" not in self.screen():
            self.keys("l"); self.wait_screen("screen=settings", 5)
        if "sidebar" in self.screen(): self.keys("right"); time.sleep(0.3)      # (D-pad test) a stray LEFT may have moved the focus
        self.keys(*(["up"] * 16), *(["down"] * SET_ROWS[name]))
        time.sleep(0.2)
    def pick(self, option):
        """The setting row is highlighted: opens its dropdown list and takes option 0.. (the cursor starts on the current value)."""
        self.keys("cross")
        ok = self.wait_screen("Choose with the D-pad", 5)
        self.keys(*(["up"] * 6), *(["down"] * option), "cross")
        return ok
    def choose(self, name, option, confirm=True):
        """Quick Save / Install received files: opens the dropdown, picks option 0..2 and confirms with [X]."""
        self.goto_setting(name)
        self.keys("cross")
        ok = self.wait_screen("Choose with the D-pad", 5)
        self.keys("up", "up", *(["down"] * option), "cross")
        if confirm:
            ok = ok and self.wait_screen("Use this setting?", 5)
            self.keys("cross")
            ok = ok and self.wait_screen("screen=settings", 5)
        return ok
    def to_browser(self):
        """From the Receive tab: Send tab -> [SQ] -> the file browser."""
        self.keys("r")
        ok = self.wait_screen("screen=send", 5)
        self.keys("square")
        return ok and self.wait_screen("Choose files or folders to send", 5)
    def finish(self):
        """Close a result dialog and get back to the Receive tab."""
        self.keys("cross")
        if wait_for(lambda: "screen=send" in self.screen() or "screen=receive" in self.screen(), 5) and "screen=send" in self.screen():
            self.keys("circle")
        return self.wait_screen("This device", 5)
    def log(self): return open(self.log_path).read()
    def config(self): return json.load(open(os.path.join(self.data, "config.json")))
    def history(self):
        p = os.path.join(self.data, "history.json")
        return json.load(open(p)) if os.path.exists(p) else []
    def stop(self):
        self.proc.terminate()
        try: self.proc.wait(5)
        except Exception: self.proc.kill()

# =========================================================================== TLS helpers
cctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
cctx.check_hostname = False
cctx.verify_mode = ssl.CERT_NONE

def raw_request(port, method, path, body=b"", headers=None, chunked=False, chunk=65536, slow=0.0, close_after=None, timeout=30):
    """One HTTPS request on a fresh connection. Returns (status, body_bytes). Optionally chunked / slow / aborted."""
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s = cctx.wrap_socket(s)
    h = {"Host": "x", "Connection": "close"}
    h.update(headers or {})
    if chunked: h["Transfer-Encoding"] = "chunked"
    else: h["Content-Length"] = str(len(body))
    head = f"{method} {path} HTTP/1.1\r\n" + "".join(f"{k}: {v}\r\n" for k, v in h.items()) + "\r\n"
    s.sendall(head.encode())
    sent = 0
    try:
        for i in range(0, len(body), chunk):
            part = body[i:i + chunk]
            if chunked: s.sendall(b"%x\r\n" % len(part) + part + b"\r\n")
            else: s.sendall(part)
            sent += len(part)
            if slow: time.sleep(slow)
            if close_after is not None and sent >= close_after:
                s.close()
                return None, b""
        if chunked: s.sendall(b"0\r\n\r\n")
    except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
        pass
    out = b""
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            out += d
    except Exception:
        pass
    s.close()
    if not out.startswith(b"HTTP/1."): return None, out
    status = int(out.split(b" ", 2)[1])
    return status, out.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in out else b""

API = "/api/localsend/v2/"
def sender_info(alias="Test Sender", fp=None):
    return {"alias": alias, "version": "2.1", "deviceModel": "Pytest", "deviceType": "desktop",
            "fingerprint": fp or sha(alias.encode()), "port": 53399, "protocol": "https", "download": False}

def file_entry(fid, name, data, with_sha=False, ftype="application/octet-stream"):
    e = {"id": fid, "fileName": name, "size": len(data), "fileType": ftype}
    if with_sha: e["sha256"] = sha(data)
    return e

def prepare(port, files, alias="Test Sender", fp=None, query="", timeout=30):
    body = json.dumps({"info": sender_info(alias, fp), "files": {f["id"]: f for f in files}}).encode()
    st, b = raw_request(port, "POST", API + "prepare-upload" + query, body, {"Content-Type": "application/json"}, timeout=timeout)
    try: j = json.loads(b) if b else None
    except Exception: j = None
    return st, j

def upload(port, sid, fid, token, data, **kw):
    return raw_request(port, "POST", API + f"upload?sessionId={sid}&fileId={fid}&token={token}", data,
                       {"Content-Type": "application/octet-stream"}, **kw)

def prepare_async(port, files, **kw):
    """prepare-upload blocks until the user decided - run it in a thread."""
    box = {}
    def run(): box["r"] = prepare(port, files, **kw)
    t = threading.Thread(target=run, daemon=True); t.start()
    return t, box


def join_answer(t, V, key, total=10):
    """The answer key can be lost when it arrives in the very frame the dialog appears (slow CI machines): press again while the sender still waits."""
    end = time.time() + total
    while t.is_alive() and time.time() < end:
        t.join(2.5)
        if t.is_alive() and V.has("Incoming request"): V.keys(key)

ALL_VITAS = []
def finish_all():
    for v in VITAS: v.stop()
VITAS = []
def sanitizer_clean(vs):
    bad = []
    for v in vs:
        log = v.log()
        if "AddressSanitizer" in log or "runtime error" in log or "UndefinedBehavior" in log or "Segmentation" in log:
            bad.append((v.name, log[-1500:]))
    return bad

# =========================================================================== RECEIVE tests
def test_receive():
    print("== RECEIVE: the Vita as the receiver")
    V = Vita("recv", 53341, 53399, config={"alias": "Recv Vita"})
    VITAS.append(V)
    port = V.port

    # ---- bad requests
    st, _ = raw_request(port, "POST", API + "prepare-upload", b"{nope", {"Content-Type": "application/json"})
    check("prepare-upload with broken JSON -> 400", st == 400, st)
    st, _ = raw_request(port, "POST", API + "prepare-upload", json.dumps({"info": sender_info(), "files": {}}).encode())
    check("prepare-upload with no files -> 400", st == 400, st)
    st, _ = raw_request(port, "POST", API + "prepare-upload", json.dumps({"files": {"a": file_entry("a", "x", b"1")}}).encode())
    check("prepare-upload without sender info -> 400", st == 400, st)
    bad = file_entry("a", "x", b"1"); bad["size"] = -5
    st, _ = prepare(port, [bad])
    check("prepare-upload with a negative size -> 400", st == 400, st)
    st, _ = raw_request(port, "GET", API + "prepare-upload")
    check("GET prepare-upload -> 405", st == 405, st)
    st, _ = raw_request(port, "POST", API + "upload?sessionId=a&fileId=b", b"abc")
    check("upload with missing token -> 400", st == 400, st)
    st, _ = raw_request(port, "POST", API + "upload?sessionId=a&fileId=b&token=c", b"abc")
    check("upload without a session -> 403", st == 403, st)
    st, _ = raw_request(port, "POST", API + "cancel?sessionId=nothing")
    check("cancel of an unknown session -> 403", st == 403, st)
    check("browser download API is refused", raw_request(port, "POST", API + "prepare-download")[0] == 403)

    # ---- user says NO
    f1, f2 = rnd(5000, 1), rnd(300000, 2)
    files = [file_entry("id-1", "hello.bin", f1), file_entry("id-2", "big.bin", f2)]
    t, box = prepare_async(port, files, alias="Banana PC")
    ok = V.wait_screen("Incoming request", 10)
    check("request dialog appears on the Vita", ok, V.screen())
    scr = V.screen()
    check("dialog names the sender, the files and the total size", "Banana PC" in scr and "hello.bin" in scr and "big.bin" in scr and "2 files" in scr, scr)
    V.keys("circle")
    join_answer(t, V, "circle")
    check("rejecting -> sender gets 403", box.get("r", (None,))[0] == 403, box)
    check("after rejecting the Vita is back on the home screen", V.wait_screen("This device", 5))
    check("rejected request is in the history", wait_for(lambda: any(h["status"] == 3 for h in V.history()), 5), V.history())

    # ---- user says YES
    t, box = prepare_async(port, files, alias="Banana PC")
    V.wait_screen("Incoming request", 10)
    V.keys("cross")
    join_answer(t, V, "cross")
    st, j = box["r"]
    check("accepting -> 200 with sessionId and one token per file",
          st == 200 and j and j.get("sessionId") and set(j.get("files", {})) == {"id-1", "id-2"}, box)
    sid, tok = j["sessionId"], j["files"]
    check("transfer screen is shown", V.wait_screen("Receiving", 5) and "Banana PC" in V.screen(), V.screen())

    st, _ = upload(port, sid, "id-1", "wrongtoken", f1)
    check("upload with a wrong token -> 403", st == 403, st)
    st, _ = upload(port, "wrongsession", "id-1", tok["id-1"], f1)
    check("upload with a wrong session -> 403", st == 403, st)
    st, _ = upload(port, sid, "id-9", tok["id-1"], f1)
    check("upload for an unknown file id -> 403", st == 403, st)

    # a second prepare while a session is open must be refused
    st, _ = prepare(port, [file_entry("z", "z.bin", b"z")], alias="Impatient")
    check("second prepare-upload while busy -> 409", st == 409, st)

    st, _ = upload(port, sid, "id-1", tok["id-1"], f1)
    check("upload #1 (Content-Length) -> 200", st == 200, st)
    p1 = os.path.join(V.recv, "hello.bin")
    check("file #1 arrived byte for byte", os.path.exists(p1) and open(p1, "rb").read() == f1)
    st, _ = upload(port, sid, "id-1", tok["id-1"], f1)
    check("uploading the same file again -> 409", st == 409, st)
    st, _ = upload(port, sid, "id-2", tok["id-2"], f2, chunked=True, chunk=40000)
    check("upload #2 (chunked transfer encoding, 300 KB) -> 200", st == 200, st)
    p2 = os.path.join(V.recv, "big.bin")
    check("file #2 arrived byte for byte", os.path.exists(p2) and open(p2, "rb").read() == f2)
    check("no .part files left behind", not [n for n in os.listdir(V.recv) if n.endswith(".part")], os.listdir(V.recv))
    check("transfer screen says the files were received", V.wait_screen("2 files received", 5), V.screen())
    V.keys("cross")
    check("[X] closes the result and returns home", V.wait_screen("This device", 5))
    h = V.history()
    check("history has the finished receive", any(x["status"] == 0 and not x["out"] and x["peer"] == "Banana PC" and x["files"] == 2 for x in h), h)

    # session is over -> new one allowed, old tokens dead
    st, _ = upload(port, sid, "id-1", tok["id-1"], f1)
    check("finished session accepts no more uploads -> 403", st == 403, st)

    # ---- hostile / odd file names
    print("== RECEIVE: file names, checksums, errors")
    evil = ["../../evil1.txt", "..\\..\\evil2.txt", "/abs/evil3.txt", "a/../../evil4.txt", 'we:ird*na?me<>|".txt',
            "dir/sub/ok.txt", "trailing. . ", "ünïcödé-日本.txt", "x" * 300 + ".txt", "a//b///c.txt", ".."]
    datas = [rnd(10 + i, 100 + i) for i in range(len(evil))]
    files = [file_entry(f"e{i}", n, datas[i]) for i, n in enumerate(evil)]
    t, box = prepare_async(port, files, alias="Mallory")
    V.wait_screen("Incoming request", 10)
    V.keys("cross"); t.join(10)
    st, j = box["r"]
    check("odd names: accepted", st == 200, box)
    for i in range(len(evil)):
        s2, _ = upload(port, j["sessionId"], f"e{i}", j["files"][f"e{i}"], datas[i])
        if s2 != 200: check(f"odd names: upload {i} ({evil[i][:20]!r})", False, s2)
    root = os.path.realpath(V.recv)
    found = []
    for dp, dn, fn in os.walk(V.dir):
        for n in fn:
            found.append(os.path.realpath(os.path.join(dp, n)))
    outside = [p for p in found if not p.startswith(root + os.sep) and not p.startswith(os.path.realpath(V.data) + os.sep)
               and os.path.basename(p) not in ("screen.txt", "screen.txt.hits", "keys", "out.log")]
    check("nothing was written outside the receive folder", not outside, outside)
    check("no 'evil' file anywhere outside", not any("evil" in os.path.basename(p) and not p.startswith(root) for p in found), found)
    names = sorted(os.path.relpath(p, root) for p in found if p.startswith(root + os.sep))
    check("'../..' prefixes were removed (evil1..evil4 sit inside the folder)", all(f"evil{i}.txt" in " ".join(names) for i in (1, 2, 3, 4)), names)
    check("folders in names are kept (dir/sub/ok.txt)", "dir/sub/ok.txt" in names, names)
    check("forbidden characters were replaced", any(n.startswith("we_ird_na_me___") for n in names), names)
    check("trailing dots/spaces removed", "trailing" in names, names)
    check("UTF-8 names survive", "ünïcödé-日本.txt" in names, names)
    check("overlong names are shortened", any(len(os.path.basename(n)) <= 130 and n.startswith("xxx") for n in names), [len(n) for n in names])
    check("'a//b///c.txt' becomes a/b/c.txt", "a/b/c.txt" in names, names)
    check("the name '..' becomes a harmless file name", "file" in names, names)
    V.keys("cross")                                      # close the result screen
    V.wait_screen("This device", 5)

    # ---- duplicates
    d1, d2, d3 = rnd(50, 7), rnd(60, 8), rnd(70, 9)
    files = [file_entry("a", "dup.txt", d1), file_entry("b", "dup.txt", d2)]
    for rnd_i in range(2):
        t, box = prepare_async(port, files, alias="Twin")
        V.wait_screen("Incoming request", 10); V.keys("cross"); t.join(10)
        st, j = box["r"]
        upload(port, j["sessionId"], "a", j["files"]["a"], d1); upload(port, j["sessionId"], "b", j["files"]["b"], d2)
        V.wait_screen("2 files received", 5); V.keys("cross"); V.wait_screen("This device", 5)
    names = sorted(n for n in os.listdir(V.recv) if n.startswith("dup"))
    check("duplicate names get ' (n)' suffixes, nothing is overwritten", names == ["dup (1).txt", "dup (2).txt", "dup (3).txt", "dup.txt"], names)

    # ---- checksum
    good, badd = rnd(20000, 21), rnd(20000, 22)
    files = [file_entry("g", "sha-good.bin", good, with_sha=True), file_entry("b", "sha-bad.bin", badd)]
    files[1]["sha256"] = sha(good)                       # wrong on purpose
    t, box = prepare_async(port, files, alias="Hasher"); V.wait_screen("Incoming request", 10); V.keys("cross"); t.join(10)
    st, j = box["r"]
    s1, _ = upload(port, j["sessionId"], "g", j["files"]["g"], good)
    s2, _ = upload(port, j["sessionId"], "b", j["files"]["b"], badd)
    check("correct SHA-256 -> 200", s1 == 200, s1)
    check("wrong SHA-256 -> 422", s2 == 422, s2)
    check("the damaged file was deleted", not os.path.exists(os.path.join(V.recv, "sha-bad.bin")) and not os.path.exists(os.path.join(V.recv, "sha-bad.bin.part")))
    check("the good file is there", os.path.exists(os.path.join(V.recv, "sha-good.bin")))
    check("session with a failed file ends as failed", V.wait_screen("Some files could not be received", 5), V.screen())
    V.keys("cross"); V.wait_screen("This device", 5)

    # ---- size mismatch / aborted upload / empty file
    data = rnd(40000, 31)
    files = [file_entry("s", "mismatch.bin", data), file_entry("t", "aborted.bin", rnd(900000, 32)), file_entry("u", "empty.bin", b"")]
    t, box = prepare_async(port, files, alias="Flaky"); V.wait_screen("Incoming request", 10); V.keys("cross"); t.join(10)
    st, j = box["r"]
    s1, _ = raw_request(port, "POST", API + f"upload?sessionId={j['sessionId']}&fileId=s&token={j['files']['s']}", data[:1000])
    s1 = s1 or 0
    check("Content-Length different from the announced size -> 400", s1 == 400, s1)
    raw_request(port, "POST", API + f"upload?sessionId={j['sessionId']}&fileId=t&token={j['files']['t']}", rnd(900000, 32), close_after=300000, chunk=50000)
    time.sleep(0.5)
    check("connection dropped mid-upload: no file and no .part left",
          not os.path.exists(os.path.join(V.recv, "aborted.bin")) and not os.path.exists(os.path.join(V.recv, "aborted.bin.part")), os.listdir(V.recv))
    s3, _ = upload(port, j["sessionId"], "u", j["files"]["u"], b"")
    check("empty file -> 200", s3 == 200, s3)
    check("empty file exists and is empty", os.path.exists(os.path.join(V.recv, "empty.bin")) and os.path.getsize(os.path.join(V.recv, "empty.bin")) == 0)
    check("session with failures ends", V.wait_screen("Some files could not be received", 25), V.screen())
    V.keys("cross"); V.wait_screen("This device", 5)

    # ---- cancel by the sender
    files = [file_entry("c1", "c1.bin", rnd(1000, 41)), file_entry("c2", "c2.bin", rnd(1000, 42))]
    t, box = prepare_async(port, files, alias="Canceller"); V.wait_screen("Incoming request", 10); V.keys("cross"); t.join(10)
    st, j = box["r"]
    st, _ = raw_request(port, "POST", API + f"cancel?sessionId={j['sessionId']}")
    check("cancel by the sender -> 200", st == 200, st)
    check("Vita shows 'Cancelled by Canceller'", V.wait_screen("Cancelled by Canceller", 5), V.screen())
    st, _ = upload(port, j["sessionId"], "c1", j["files"]["c1"], rnd(1000, 41))
    check("upload after cancel -> 403", st == 403, st)
    V.keys("cross"); V.wait_screen("This device", 5)

    # ---- cancel on the Vita while data is flowing
    big = rnd(600000, 51)
    files = [file_entry("k", "slow.bin", big)]
    t, box = prepare_async(port, files, alias="Slowpoke"); V.wait_screen("Incoming request", 10); V.keys("cross"); t.join(10)
    st, j = box["r"]
    up = {}
    def slow_upload():
        up["r"] = upload(port, j["sessionId"], "k", j["files"]["k"], big, chunk=20000, slow=0.15)
    ut = threading.Thread(target=slow_upload, daemon=True); ut.start()
    V.wait_screen("Receiving", 5); time.sleep(0.6)
    V.keys("circle")
    check("[O] asks for confirmation", V.wait_screen("Cancel the running transfer?", 5), V.screen())
    V.keys("cross")
    ut.join(20)
    check("cancelling on the Vita aborts the upload", up.get("r", (200,))[0] != 200, up)
    check("Vita shows the transfer as cancelled", V.wait_screen("Cancelled on this device", 8), V.screen())
    check("partial file removed", not os.path.exists(os.path.join(V.recv, "slow.bin")) and not os.path.exists(os.path.join(V.recv, "slow.bin.part")))
    V.keys("cross"); V.wait_screen("This device", 5)

    # ---- text message
    t, box = prepare_async(port, [dict(file_entry("m", "message.txt", b"Hello from the PC\nsecond line", ftype="text/plain"), preview="Hello from the PC\nsecond line")], alias="Texter")
    check("text message: dialog shows the text", V.wait_screen("Incoming request", 10) and "Hello from the PC" in V.screen(), V.screen())
    V.keys("cross"); t.join(10)
    check("text message accepted -> 204 (nothing to download)", box["r"][0] == 204, box)
    check("message screen appears with the text", V.wait_screen("Message from Texter", 8) and "second line" in V.screen(), V.screen())
    check("message is in the history with its text", any(h["isText"] and h["text"].startswith("Hello from the PC") for h in V.history()), V.history())
    V.keys("circle"); V.wait_screen("This device", 5)

    # ---- settings: name, accept mode, favorites, PIN
    print("== RECEIVE: settings (name, accept mode, favorites, PIN, folder)")
    V.keys("l")
    check("settings tab opens with [L]", V.wait_screen("screen=settings", 5), V.screen())
    V.keys("cross")                                     # device name -> keyboard (starts with the current name)
    check("name keyboard opens with the current name", V.wait_screen("screen=keyboard", 5) and "Recv Vita" in V.screen(), V.screen())
    V.keys(*(["square"] * 12))
    V.keys(*osk_keys("Pico 1"), "start")
    check("device name changed through the on-screen keyboard", wait_for(lambda: V.config().get("alias") == "Pico 1", 40), V.config())
    check("new name is shown in the settings list", V.wait_screen("Pico 1", 5), V.screen())
    # a name with accents, Cyrillic and Greek letters (the keyboard has pages for them), and deleting one letter
    V.keys("up", "up", "cross"); V.wait_screen("screen=keyboard", 5)
    V.keys(*(["square"] * 6))
    V.keys(*osk_keys_unicode("Zoë Привет Αθ\bά"), "start")
    check("accented, Cyrillic and Greek letters can be typed (and deleted as whole characters)",
          wait_for(lambda: V.config().get("alias") == "Zoë Привет Αά", 60), V.config())
    check("the name with those letters is shown", V.wait_screen("Zoë Привет Αά", 5), V.screen())
    V.wait_screen("screen=settings", 5); time.sleep(1.0)        # (the keyboard's key queue must be fully drained)
    V.choose("accept", 1)
    check("accept mode switches to 'favorites' (after the question)", wait_for(lambda: V.config().get("autoAccept") == 1, 8), V.config())
    V.keys("circle")
    check("[O] leaves the settings", V.wait_screen("This device", 5))

    t, box = prepare_async(port, [file_entry("f", "fav1.txt", b"x")], alias="Friend", fp="aa" * 32)
    check("favorites mode: a stranger still gets asked", V.wait_screen("Incoming request", 10), V.screen())
    V.keys("square"); t.join(10)                        # accept + make favorite
    st, j = box["r"]
    check("[SQ] accepts and remembers the sender as a favorite",
          st == 200 and wait_for(lambda: any(f["fingerprint"] == "aa" * 32 for f in V.config().get("favorites", [])), 5), (box, V.config()))
    upload(port, j["sessionId"], "f", j["files"]["f"], b"x")
    V.wait_screen("1 file received", 5); V.keys("cross"); V.wait_screen("This device", 5)
    st, j = prepare(port, [file_entry("f", "fav2.txt", b"y")], alias="Friend", fp="aa" * 32, timeout=5)
    check("a favorite is accepted automatically (no dialog)", st == 200 and j and j.get("sessionId"), (st, j))
    st2, _ = prepare(port, [file_entry("f", "other.txt", b"y")], alias="Stranger", fp="bb" * 32, timeout=3)
    check("while that session runs, others get 409", st2 == 409, st2)
    raw_request(port, "POST", API + f"cancel?sessionId={j['sessionId']}")
    V.wait_screen("Cancelled by Friend", 5); V.keys("cross"); V.wait_screen("This device", 5)

    V.choose("accept", 2)
    check("accept mode switches to 'everyone'", wait_for(lambda: V.config().get("autoAccept") == 2, 8), V.config())
    V.keys("circle"); V.wait_screen("This device", 5)
    st, j = prepare(port, [file_entry("f", "auto.txt", b"auto")], alias="Anyone", fp="cc" * 32, timeout=5)
    check("'everyone' mode: accepted without asking", st == 200 and j, (st, j))
    upload(port, j["sessionId"], "f", j["files"]["f"], b"auto")
    check("auto-accepted file arrived", wait_for(lambda: os.path.exists(os.path.join(V.recv, "auto.txt")), 5))
    V.wait_screen("1 file received", 5); V.keys("cross"); V.wait_screen("This device", 5)

    V.choose("accept", 0)
    check("accept mode switches back to 'ask'", wait_for(lambda: V.config().get("autoAccept") == 0, 8), V.config())
    V.goto_setting("pin"); V.keys("cross")               # PIN item -> number keyboard
    check("PIN keyboard opens", V.wait_screen("PIN (digits only", 5), V.screen())
    V.keys(*osk_keys("4321", digits=True), "start")
    check("PIN stored", wait_for(lambda: V.config().get("pin") == "4321", 12), V.config())
    V.keys("circle"); V.wait_screen("This device", 5)

    files = [file_entry("p", "pin.txt", b"pin")]
    st, _ = prepare(port, files, alias="PinUser")
    check("PIN set: prepare without PIN -> 401", st == 401, st)
    st, _ = prepare(port, files, alias="PinUser", query="?pin=0000")
    check("PIN set: wrong PIN -> 401", st == 401, st)
    t, box = prepare_async(port, files, alias="PinUser", query="?pin=4321")
    check("PIN set: right PIN -> the dialog appears", V.wait_screen("Incoming request", 10), V.screen())
    V.keys("cross"); t.join(10)
    st, j = box["r"]
    check("PIN set: right PIN -> 200", st == 200, box)
    upload(port, j["sessionId"], "p", j["files"]["p"], b"pin")
    V.wait_screen("1 file received", 5); V.keys("cross"); V.wait_screen("This device", 5)

    V.goto_setting("pin"); V.keys("square")
    check("[SQ] on the PIN item removes the PIN", wait_for(lambda: V.config().get("pin") == "", 8), V.config())
    V.keys("circle"); V.wait_screen("This device", 5)

    # receive folder through the browser
    newdir = os.path.join(ROOT, "elsewhere"); os.makedirs(os.path.join(newdir, "target"))
    V.stop(); VITAS.remove(V)
    V = Vita("recv2", 53342, 53399, config={"alias": "Folder Test"}, browse=newdir)
    VITAS.append(V)
    V.goto_setting("folder"); V.keys("cross")        # settings -> 'Save received files in' -> browser (starts in newdir)
    check("folder picker opens in the start folder", V.wait_screen("Choose the folder for received files", 8) and "target/" in V.screen(), V.screen())
    V.keys("cross", "triangle")                         # enter target/, use this folder
    check("receive folder saved", wait_for(lambda: V.config().get("receiveDir", "").endswith("elsewhere/target"), 12), V.config())
    V.keys("circle")
    t, box = prepare_async(V.port, [file_entry("f", "moved.txt", b"m")], alias="Mover"); V.wait_screen("Incoming request", 10); V.keys("cross"); t.join(10)
    st, j = box["r"]
    upload(V.port, j["sessionId"], "f", j["files"]["f"], b"m")
    check("files now land in the chosen folder", wait_for(lambda: os.path.exists(os.path.join(newdir, "target", "moved.txt")), 5))
    V.wait_screen("1 file received", 5); V.keys("cross"); V.wait_screen("This device", 5)
    V.stop(); VITAS.remove(V)

    # ---- PIN brute force (separate instance, because the lock lasts a minute)
    V = Vita("recv3", 53343, 53399, config={"pin": "2468", "alias": "Locked"})
    VITAS.append(V)
    files = [file_entry("p", "pin.txt", b"pin")]
    codes = [prepare(V.port, files, query=f"?pin=000{i}")[0] for i in range(5)]
    check("wrong PINs are refused with 401", codes == [401] * 5, codes)
    st, _ = prepare(V.port, files, query="?pin=2468")
    check("after 5 wrong PINs even the right one is blocked with 429", st == 429, st)
    V.stop(); VITAS.remove(V)

    # ---- sanity: the program survived all of that
    print("== RECEIVE: health")
    time.sleep(0.5)
    bad = sanitizer_clean(ALL_VITAS)
    check("no sanitizer / crash messages in any run so far", not bad, bad)

# =========================================================================== EXTRA tests
def test_extra():
    print("== EXTRA: big file, parallel uploads, unusable folder")
    V = Vita("extra", 53361, 53399, config={"alias": "Extra", "autoAccept": 2})
    VITAS.append(V)
    big = rnd(25 * 1024 * 1024, 81)
    t0 = time.time()
    st, j = prepare(V.port, [file_entry("big", "big.bin", big, with_sha=True)], alias="Bulk")
    check("auto-accepted 25 MB request -> 200", st == 200 and j, st)
    st, _ = upload(V.port, j["sessionId"], "big", j["files"]["big"], big, chunk=262144, timeout=120)
    dt = time.time() - t0
    check("25 MB upload with SHA-256 -> 200", st == 200, st)
    got = open(os.path.join(V.recv, "big.bin"), "rb").read() if os.path.exists(os.path.join(V.recv, "big.bin")) else b""
    check("25 MB arrived intact", sha(got) == sha(big), len(got))
    print(f"        ({len(big) / dt / 1e6:.1f} MB/s on this PC, sanitizer build)")
    V.wait_screen("1 file received", 5); V.keys("cross"); V.wait_screen("This device", 5)

    datas = [rnd(400000 + i, 90 + i) for i in range(4)]
    files = [file_entry(f"p{i}", f"par{i}.bin", datas[i], with_sha=True) for i in range(4)]
    st, j = prepare(V.port, files, alias="Parallel")
    res = {}
    def up(i):
        res[i] = upload(V.port, j["sessionId"], f"p{i}", j["files"][f"p{i}"], datas[i], chunk=50000, slow=0.01)[0]
    ths = [threading.Thread(target=up, args=(i,)) for i in range(4)]
    [t.start() for t in ths]; [t.join(60) for t in ths]
    check("4 uploads at the same time all succeed", st == 200 and res == {0: 200, 1: 200, 2: 200, 3: 200}, (st, res))
    check("all 4 files intact", all(os.path.exists(os.path.join(V.recv, f"par{i}.bin")) and
                                     open(os.path.join(V.recv, f"par{i}.bin"), "rb").read() == datas[i] for i in range(4)))
    check("session ended after the last one", V.wait_screen("4 files received", 8), V.screen())
    V.keys("cross"); V.wait_screen("This device", 5)

    # a file that is announced bigger than what is sent, many files, unusual sizes
    many = [file_entry(f"m{i}", f"many/{i:03d}.txt", b"x" * i) for i in range(120)]
    st, j = prepare(V.port, many, alias="Many")
    check("120 files in one request -> 200 with 120 tokens", st == 200 and j and len(j["files"]) == 120, st)
    ok = 0
    for i in range(120):
        s2, _ = upload(V.port, j["sessionId"], f"m{i}", j["files"][f"m{i}"], b"x" * i)
        ok += s2 == 200
    check("all 120 uploads succeed", ok == 120, ok)
    check("120 files on disk", len(os.listdir(os.path.join(V.recv, "many"))) == 120)
    check("session ends and shows 120 files", V.wait_screen("120 files received", 8), V.screen())
    V.keys("cross"); V.wait_screen("This device", 5)
    V.stop(); VITAS.remove(V)

    # the usual port is taken by another copy -> the next free port is used and announced
    V1 = Vita("dup1", 53371, 53399)
    VITAS.append(V1)
    V2 = Vita("dup2", 53371, 53399, config={"alias": "Second", "autoAccept": 2})
    VITAS.append(V2)
    check("second copy on a taken port moves to the next port", V2.wait_screen("53372", 8) and "was not available" in V2.log(), V2.screen())
    st, j = prepare(53372, [file_entry("a", "dup-port.txt", b"hi")], alias="PortTest")
    check("and is reachable there", st == 200 and j, st)
    raw_request(53372, "POST", API + f"cancel?sessionId={j['sessionId']}")
    st, b = raw_request(53372, "POST", API + "register", json.dumps(sender_info("Reg Test")).encode(), {"Content-Type": "application/json"})
    check("/register on the new port answers with that port", st == 200 and json.loads(b).get("port") == 53372, b)
    V2.keys("cross"); time.sleep(0.3)
    V1.stop(); VITAS.remove(V1); V2.stop(); VITAS.remove(V2)

    # ten ports in a row are taken -> the system picks any free port, and that port is what gets announced
    holders = []
    for p in range(53380, 53390):
        h = socket.socket(); h.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        h.bind(("0.0.0.0", p)); h.listen(1); holders.append(h)
    V = Vita("anyport", 53380, 53399, config={"alias": "AnyPort", "autoAccept": 2})
    VITAS.append(V)
    import re
    def shown_port():
        m = re.search(r":(\d+) \u00b7 https", V.screen())
        return int(m.group(1)) if m else 0
    check("all 10 usual ports busy: a free port is chosen by the system", wait_for(lambda: shown_port() not in (0, 53380), 8) and "was not available" in V.log(), (V.screen(), V.log()[-400:]))
    anyport = shown_port()
    check("the chosen port is outside the blocked range", not (53380 <= anyport < 53390), anyport)
    st, j = prepare(anyport, [file_entry("a", "any.txt", b"hi")], alias="AnyTest")
    check("and the Vita is reachable there", st == 200 and j, st)
    if j: raw_request(anyport, "POST", API + f"cancel?sessionId={j['sessionId']}")
    V.stop(); VITAS.remove(V)
    for h in holders: h.close()

    # not enough room on the card
    V = Vita("lowspace", 53363, 53399, config={"alias": "Tiny Card", "autoAccept": 2}, extra=["--free-space", str(3 * 1024 * 1024)])
    VITAS.append(V)
    st, _ = prepare(V.port, [file_entry("a", "huge.bin", b"x" * (5 * 1024 * 1024))], alias="Greedy")
    check("files that do not fit -> 507 before anything is written", st == 507, st)
    check("the Vita says so on screen", V.wait_screen("Not enough space", 5), V.screen())
    check("and the history records it", any("not enough space" in h["summary"] for h in V.history()), V.history())
    V.keys("cross"); V.wait_screen("This device", 5)
    st, j = prepare(V.port, [file_entry("a", "fits.bin", b"x" * 1000)], alias="Modest")
    check("files that fit are accepted as usual", st == 200 and j, st)
    if j: raw_request(V.port, "POST", API + f"cancel?sessionId={j['sessionId']}")
    V.stop(); VITAS.remove(V)

    # a receive folder that cannot be created
    V = Vita("badfolder", 53362, 53399, config={"alias": "Broken", "autoAccept": 2, "receiveDir": "/proc/nope/recv"})
    VITAS.append(V)
    st, j = prepare(V.port, [file_entry("a", "a.txt", b"a")], alias="Hopeful")
    check("receive folder cannot be created -> error status (not a crash)", st in (500, 507), (st, j))
    check("the Vita says why", V.wait_screen("Cannot create folder", 5), V.screen())
    V.keys("cross"); V.wait_screen("This device", 5)
    st, j = prepare(V.port, [file_entry("a", "a.txt", b"a")], alias="Hopeful")
    check("and is not stuck afterwards (second try gets the same clean error)", st in (500, 507), st)
    time.sleep(0.5)
    bad = sanitizer_clean(ALL_VITAS)
    check("no sanitizer / crash messages", not bad, bad)
    V.stop(); VITAS.remove(V)

# =========================================================================== SEND tests
RECV_CERT = {}
def make_cert():
    key, crt = os.path.join(ROOT, "rx.key"), os.path.join(ROOT, "rx.crt")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", crt, "-days", "30",
                    "-subj", "/CN=Fake Phone"], check=True, stderr=subprocess.DEVNULL)
    der = ssl.PEM_cert_to_DER_cert(open(crt).read())
    return key, crt, sha(der)

class Receiver:
    """A fake LocalSend device that the Vita sends to."""
    def __init__(self, port, alias="Fake Phone", chunked=False):
        key, crt, fp = make_cert()
        self.port, self.alias, self.fp, self.chunked = port, alias, fp, chunked
        self.info = {"alias": alias, "version": "2.1", "deviceModel": "Pixel", "deviceType": "mobile",
                     "fingerprint": fp, "port": port, "protocol": "https", "download": False}
        self.prepares, self.uploads, self.cancels, self.registers = [], {}, [], []
        self.pin = None
        self.fail_ids = set()           # upload of these file ids is answered with a 500
        self.mode = "accept"            # accept | reject | busy | text204 | skip_last | fail_upload | slow
        self.slow_delay = 0.0
        me = self
        class H(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            def log_message(self, *a): pass
            def reply(self, code, obj=None):
                body = b"" if obj is None else json.dumps(obj).encode()
                self.send_response(code)
                if code != 204: self.send_header("Content-Type", "application/json")
                self.send_header("Connection", "close")
                if me.chunked and body:
                    self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
                    h = len(body) // 2
                    for p in (body[:h], body[h:]): self.wfile.write(b"%x\r\n" % len(p) + p + b"\r\n")
                    self.wfile.write(b"0\r\n\r\n")
                else:
                    self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
                self.close_connection = True
            def do_POST(self):
                u = urlparse(self.path); q = parse_qs(u.query)
                n = int(self.headers.get("Content-Length", 0))
                if u.path.endswith("/register"):
                    me.registers.append(json.loads(self.rfile.read(n))); return self.reply(200, me.info)
                if u.path.endswith("/prepare-upload"):
                    body = json.loads(self.rfile.read(n))
                    me.prepares.append((body, q))
                    if me.pin is not None and q.get("pin", [None])[0] != me.pin: return self.reply(401, {"message": "PIN required"})
                    if me.mode == "reject": return self.reply(403, {"message": "Rejected"})
                    if me.mode == "busy": return self.reply(409, {"message": "Blocked by another session"})
                    files = body["files"]
                    if me.mode == "text204" and len(files) == 1 and "preview" in next(iter(files.values())): return self.reply(204)
                    ids = list(files)
                    if me.mode == "skip_last": ids = ids[:-1]
                    return self.reply(200, {"sessionId": "sess-1", "files": {i: "tok-" + i for i in ids}})
                if u.path.endswith("/upload"):
                    fid, tok = q.get("fileId", [""])[0], q.get("token", [""])[0]
                    data = b""
                    while len(data) < n:
                        part = self.rfile.read(min(65536, n - len(data)))
                        if not part: break
                        data += part
                        if me.mode == "slow": time.sleep(me.slow_delay)
                    ok = q.get("sessionId", [""])[0] == "sess-1" and tok == "tok-" + fid
                    if me.mode == "fail_upload" or fid in me.fail_ids: return self.reply(500, {"message": "disk on fire"})
                    if not ok: return self.reply(403, {"message": "bad token"})
                    me.uploads[fid] = data
                    return self.reply(200)
                if u.path.endswith("/cancel"):
                    me.cancels.append(q.get("sessionId", [""])[0]); return self.reply(200)
                self.reply(404, {"message": "nope"})
            def do_GET(self):
                self.reply(200, {k: v for k, v in me.info.items() if k not in ("port", "protocol")})
        self.srv = ThreadingHTTPServer(("127.0.0.1", port), H)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(crt, key)
        self.srv.socket = ctx.wrap_socket(self.srv.socket, server_side=True)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
    def reset(self):
        self.prepares.clear(); self.uploads.clear(); self.cancels.clear(); self.pin = None; self.fail_ids = set(); self.mode = "accept"; self.slow_delay = 0
    def stop(self):
        self.srv.shutdown()
        self.srv.server_close()

def announce_to(vita, info):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.sendto(json.dumps(dict(info, announce=False)).encode(), ("127.0.0.1", vita.port))
    s.close()

def test_send():
    print("== SEND: the Vita as the sender")
    # test files
    src = os.path.join(ROOT, "tosend"); os.makedirs(os.path.join(src, "album", "sub"))
    A, B = rnd(120000, 61), rnd(777, 62)
    C, D = rnd(5000, 63), rnd(333, 64)
    open(os.path.join(src, "a-photo.jpg"), "wb").write(A)
    open(os.path.join(src, "b-note.txt"), "wb").write(B)
    open(os.path.join(src, "album", "c.bin"), "wb").write(C)
    open(os.path.join(src, "album", "sub", "d.bin"), "wb").write(D)
    R = Receiver(53352)
    V = Vita("send", 53351, 53352, config={"alias": "Sender Vita"}, browse=src)
    VITAS.append(V)
    announce_to(V, R.info)

    # ---- no devices / browser basics
    V.to_browser()
    check("Send tab -> [SQ] opens the file browser at the start folder", V.has("a-photo.jpg"), V.screen())
    scr = V.screen()
    order = [scr.find(n) for n in ("album/", "a-photo.jpg", "b-note.txt")]
    check("folders are listed first, then files, alphabetically", order == sorted(order) and -1 not in order, scr)
    check("file sizes are shown", "117.2 KB" in scr and "777 B" in scr, scr)

    # ---- send two files: a-photo.jpg + b-note.txt (cursor is on 'album/'; move down)
    V.keys("down", "cross", "down", "cross")
    check("[X] on a file selects it", wait_for(lambda: "2 selected" in V.screen(), 6), V.screen())
    V.keys("triangle")
    check("[TRI] goes back to the Send tab with the selection and lists the fake phone", V.wait_screen("2 items selected", 5) and V.wait_screen("Fake Phone", 8), V.screen())
    V.keys("cross")
    check("transfer finishes", V.wait_screen("Sent 2 files to Fake Phone", 20), V.screen())
    check("receiver got a valid prepare-upload", len(R.prepares) == 1, R.prepares)
    body, q = R.prepares[0]
    check("prepare-upload 'info' identifies the Vita", body["info"]["alias"] == "Sender Vita" and body["info"]["fingerprint"] == V.fp
          and body["info"]["deviceType"] == "mobile" and body["info"]["version"].startswith("2."), body["info"])
    fs = body["files"]
    check("file entries carry id/fileName/size/fileType and the key equals the id",
          all(k == v["id"] and {"fileName", "size", "fileType"} <= set(v) for k, v in fs.items()) and len(fs) == 2, fs)
    byname = {v["fileName"]: v for v in fs.values()}
    check("names, sizes and MIME types are right", byname["a-photo.jpg"]["size"] == len(A) and byname["a-photo.jpg"]["fileType"] == "image/jpeg"
          and byname["b-note.txt"]["size"] == len(B) and byname["b-note.txt"]["fileType"] == "text/plain", byname)
    ids = {v["fileName"]: k for k, v in fs.items()}
    check("uploaded bytes are identical", R.uploads.get(ids["a-photo.jpg"]) == A and R.uploads.get(ids["b-note.txt"]) == B,
          {k: len(v) for k, v in R.uploads.items()})
    check("history has the finished send", wait_for(lambda: any(h["out"] and h["status"] == 0 and h["files"] == 2 and h["peer"] == "Fake Phone" for h in V.history()), 5), V.history())
    V.finish()

    # ---- a whole folder (recursive)
    R.reset()
    V.to_browser()
    V.keys("square")                                    # 'album/' is under the cursor -> select folder
    V.keys("triangle")
    V.wait_screen("screen=send", 5); V.wait_screen("Fake Phone", 8)
    V.keys("cross")
    check("folder transfer finishes", V.wait_screen("Sent 2 files to Fake Phone", 20), V.screen())
    names = sorted(v["fileName"] for v in R.prepares[0][0]["files"].values()) if R.prepares else []
    check("folder contents are sent with relative paths", names == ["album/c.bin", "album/sub/d.bin"], names)
    byname = {v["fileName"]: k for k, v in R.prepares[0][0]["files"].items()}
    check("nested file bytes identical", R.uploads.get(byname["album/c.bin"]) == C and R.uploads.get(byname["album/sub/d.bin"]) == D)
    V.finish()

    # ---- receiver rejects
    R.reset(); R.mode = "reject"
    V.to_browser()
    V.keys("down", "down", "triangle")                  # cursor on b-note.txt (after album/, a-photo.jpg): [TRI] selects it and goes on
    V.wait_screen("screen=send", 5); V.wait_screen("Fake Phone", 8); V.keys("cross")
    check("receiver says no -> clear message on the Vita", V.wait_screen("the other device said no", 15), V.screen())
    check("rejected send is in the history", wait_for(lambda: any(h["out"] and h["status"] == 3 for h in V.history()), 5), V.history())
    V.finish()

    # ---- receiver is busy
    R.reset(); R.mode = "busy"
    V.to_browser()
    V.keys("down", "down", "triangle"); V.wait_screen("screen=send", 5); V.wait_screen("Fake Phone", 8); V.keys("cross")
    check("receiver busy (409) -> clear message", V.wait_screen("busy with another transfer", 15), V.screen())
    V.finish()

    # ---- receiver accepts only some files
    R.reset(); R.mode = "skip_last"
    V.to_browser()
    V.keys("down", "cross", "down", "cross", "triangle")
    V.wait_screen("screen=send", 5); V.wait_screen("Fake Phone", 8); V.keys("cross")
    check("partially accepted: transfer completes with 1 file", V.wait_screen("Sent 1 file to Fake Phone", 20), V.screen())
    check("only the accepted file was uploaded", len(R.uploads) == 1, R.uploads.keys())
    V.finish()

    # ---- upload error
    R.reset(); R.mode = "fail_upload"
    V.to_browser()
    V.keys("down", "down", "triangle"); V.wait_screen("screen=send", 5); V.wait_screen("Fake Phone", 8); V.keys("cross")
    check("upload error (500) -> failure shown", V.wait_screen("Failed on b-note.txt", 15), V.screen())
    check("the Vita tells the receiver the session is cancelled", wait_for(lambda: R.cancels == ["sess-1"], 5), R.cancels)
    V.finish()

    # ---- PIN
    R.reset(); R.pin = "1234"
    V.to_browser()
    V.keys("down", "down", "triangle"); V.wait_screen("screen=send", 5); V.wait_screen("Fake Phone", 8); V.keys("cross")
    check("receiver wants a PIN -> keyboard opens", V.wait_screen("The other device needs a PIN", 15), V.screen())
    V.keys(*osk_keys("1234", digits=True), "start")
    check("PIN accepted -> transfer completes", V.wait_screen("Sent 1 file to Fake Phone", 20), V.screen())
    check("PIN was sent as a query parameter", any(q.get("pin") == ["1234"] for _, q in R.prepares), R.prepares)
    V.finish()
    R.reset(); R.pin = "9999"
    V.to_browser()
    V.keys("down", "down", "triangle"); V.wait_screen("screen=send", 5); V.wait_screen("Fake Phone", 8); V.keys("cross")
    V.wait_screen("The other device needs a PIN", 15)
    V.keys(*osk_keys("1111", digits=True), "start")
    check("wrong PIN -> asks again", wait_for(lambda: V.screen().count("The other device needs a PIN") and len(R.prepares) >= 2 and V.has("PIN"), 20), V.screen())
    V.keys("circle")                                    # give up
    check("giving up at the PIN prompt ends the transfer", V.wait_screen("No PIN entered", 15) or V.wait_screen("Cancelled", 5), V.screen())
    V.finish()

    # ---- text message, receiver answers 204
    R.reset(); R.mode = "text204"
    V.keys("r"); V.wait_screen("screen=send", 5)
    V.keys("triangle")
    check("[TRI] on the Send tab opens the keyboard for a message", V.wait_screen("Message to send", 5), V.screen())
    V.keys(*osk_keys("Hi Vita 42!"), "start")
    check("then the Send tab shows the message and the devices", V.wait_screen("Text message", 15) and V.wait_screen("Fake Phone", 8), V.screen())
    V.keys("cross")
    check("text message delivered (receiver replied 204)", V.wait_screen("Message delivered to Fake Phone", 15), V.screen())
    body = R.prepares[0][0]; f = next(iter(body["files"].values()))
    check("text is sent as a text/plain file with a preview", f["fileType"] == "text/plain" and f["preview"] == "Hi Vita 42!" and f["size"] == 11, f)
    check("sent message is in the history", any(h["out"] and h["isText"] and h["text"] == "Hi Vita 42!" for h in V.history()), V.history())
    V.finish()

    # ---- text message, receiver answers 200 + token (wants the bytes)
    R.reset()
    V.keys("r"); V.wait_screen("screen=send", 5); V.keys("triangle"); V.wait_screen("Message to send", 5)
    V.keys(*osk_keys("again"), "start"); V.wait_screen("Text message", 15); V.wait_screen("Fake Phone", 8); V.keys("cross")
    check("text message via upload (receiver answered 200)", V.wait_screen("Message delivered to Fake Phone", 15), V.screen())
    check("the text bytes were uploaded", R.uploads.get("msg") == b"again", R.uploads)
    V.finish()

    # ---- favorite from the picker
    V.keys("r"); V.wait_screen("screen=send", 5); V.keys("triangle"); V.wait_screen("Message to send", 5); V.keys(*osk_keys("x"), "start")
    V.wait_screen("Text message", 15); V.wait_screen("Fake Phone", 8)
    V.keys("start")
    check("[START] on a device in the Send tab marks it as favorite (D-pad test)", wait_for(lambda: any(f["fingerprint"] == R.fp for f in V.config().get("favorites", [])), 6), V.config())
    V.keys("circle"); V.wait_screen("This device", 5)

    # ---- cancel while sending
    R.reset(); R.mode = "slow"; R.slow_delay = 0.25
    open(os.path.join(src, "zz-big.bin"), "wb").write(rnd(2_000_000, 71))
    V.to_browser()
    V.keys("down", "down", "down", "down", "triangle")  # zz-big.bin is the last entry
    V.wait_screen("screen=send", 5); V.wait_screen("Fake Phone", 8); V.keys("cross")
    V.wait_screen("Sending", 10); time.sleep(1.5)
    V.keys("circle")
    check("[O] asks for confirmation while sending", V.wait_screen("Cancel the running transfer?", 5), V.screen())
    V.keys("cross")
    check("cancelled send is reported", V.wait_screen("Cancelled", 15), V.screen())
    check("receiver was told to cancel", wait_for(lambda: R.cancels == ["sess-1"], 10), R.cancels)
    V.finish()

    # ---- receiver went away
    R.stop()
    V.to_browser()
    V.keys("down", "down", "triangle"); V.wait_screen("screen=send", 5); V.wait_screen("Fake Phone", 8); V.keys("cross")
    check("unreachable receiver -> 'could not connect'", V.wait_screen("could not connect", 25), V.screen())
    V.finish()

    # ---- history screen
    V.keys("r", "r")
    check("history tab lists sends and text", V.wait_screen("screen=history", 5) and "Sent" in V.screen() and "message: Hi Vita 42!" in V.screen(), V.screen())
    V.keys("circle")
    check("program still running", V.proc.poll() is None)

    # a device that does not show up by itself: add it by IP address
    R3 = Receiver(53356, alias="Hidden Phone")
    V.to_browser()
    V.keys("down", "down", "triangle"); V.wait_screen("screen=send", 5)
    V.keys("select")
    check("[SELECT] on the Send tab opens the address keyboard", V.wait_screen("IP of the other device", 5), V.screen())
    V.keys(*osk_keys("999.1.1.1"), "start")
    check("a wrong address is refused with a hint", V.wait_screen("not an IP address", 25), V.screen())
    V.keys(*(["square"] * 12))                         # the wrong text stays in the keyboard: delete it
    V.keys(*osk_keys("127.0.0.1:53390"), "start")
    check("an address where nothing listens: clear message", V.wait_screen("Nothing answered", 25), V.screen())
    V.keys("select"); V.wait_screen("IP of the other device", 5)
    V.keys(*osk_keys("127.0.0.1:53356"), "start")
    check("a real device is added by address", V.wait_screen("Hidden Phone", 25), V.screen())
    check("and the other device was told about us (register)", len(R3.registers) == 1 and R3.registers[0]["alias"] == "Sender Vita", R3.registers)
    for _ in range(5):
        row = [l for l in V.screen().splitlines() if l.startswith(">")]
        if row and "Hidden Phone" in row[0]: break
        V.keys("down"); time.sleep(0.5)
    V.keys("cross")
    check("sending to the manually added device works", V.wait_screen("Sent 1 file to Hidden Phone", 20) and len(R3.uploads) == 1, V.screen())
    V.finish()
    R3.stop()

    # chunked replies (Dart style) from the receiver
    R2 = Receiver(53354, alias="Dart Phone", chunked=True)
    announce_to(V, R2.info)
    V.to_browser()
    V.keys("down", "down", "triangle"); V.wait_screen("screen=send", 5); V.wait_screen("Dart Phone", 8)
    for _ in range(4):
        row = [l for l in V.screen().splitlines() if l.startswith(">")]
        if row and "Dart Phone" in row[0]: break
        V.keys("down"); time.sleep(0.5)
    V.keys("cross")
    check("receiver using chunked replies works too", V.wait_screen("Sent 1 file to Dart Phone", 20), V.screen())
    V.keys("cross")
    time.sleep(0.5)
    bad = sanitizer_clean([V])
    check("no sanitizer / crash messages in the sending Vita", not bad, bad)
    V.stop(); VITAS.remove(V)

# =========================================================================== INSTALL tests (.vpk installer, Files tab)
def make_sfo(d):
    keys = sorted(d)
    kt, offs = b"", []
    for k in keys:
        offs.append(len(kt)); kt += k.encode() + b"\0"
    while len(kt) % 4: kt += b"\0"
    dt, ents = b"", b""
    for i, k in enumerate(keys):
        v = d[k].encode() + b"\0"
        mx = max(len(v), 16)
        ents += struct.pack("<HHIII", offs[i], 0x0204, len(v), mx, len(dt))
        dt += v.ljust(mx, b"\0")
    ko = 20 + 16 * len(keys)
    return b"\0PSF" + struct.pack("<IIII", 0x101, ko, ko + len(kt), len(keys)) + ents + kt + dt

def make_eboot(unsafe=False, seed=5, size=6000):
    h = bytearray(b"SCE\0") + bytearray(0x7C)
    h += struct.pack("<Q", 0x2F00000000000003 if unsafe else 0x2F00000000000002)
    return bytes(h) + rnd(size, seed)

def make_vpk(path, title="Test App", tid="TEST00001", ver="01.00", unsafe=False, method=zipfile.ZIP_DEFLATED, extra=None,
             eboot=True, cid=None, sfo=True, evil=None, nfiles=0, corrupt=False):
    """Builds a .vpk; returns {name: bytes} of everything in it (for comparing after the install)."""
    content = {}
    if sfo:
        d = {"TITLE": title, "TITLE_ID": tid, "APP_VER": ver}
        if cid: d["CONTENT_ID"] = cid
        content["sce_sys/param.sfo"] = make_sfo(d)
    if eboot: content["eboot.bin"] = make_eboot(unsafe)
    content["sce_sys/icon0.png"] = rnd(3000, 7)
    content["sce_sys/livearea/contents/template.xml"] = (b"<livearea>hello hello hello</livearea>\n" * 400)
    content["data/big.dat"] = b"".join(random.Random(i).choice([rnd(300, i), b"abcdef" * 200]) for i in range(2500))
    content.update(extra or {})
    for i in range(nfiles): content[f"many/f{i:04d}.txt"] = (b"file %d\n" % i) * 50
    with zipfile.ZipFile(path, "w", method) as z:
        for n, b in content.items():
            z.writestr(n, b, compress_type=method if len(b) > 100 else zipfile.ZIP_STORED)
        z.writestr(zipfile.ZipInfo("sce_sys/"), b"")
        if evil: z.writestr(evil, b"owned")
    if corrupt:
        raw = bytearray(open(path, "rb").read())
        i = raw.find(b"hello hello") if method == zipfile.ZIP_STORED else 200
        raw[i + 3] ^= 0x55
        open(path, "wb").write(raw)
    return content

def files_row(V, name):
    """Moves the Files cursor onto `name` (the folder listing is sorted like the program sorts it); True if found."""
    names = sorted(os.listdir(V.recv), key=str.lower)
    idx = names.index(name)
    V.keys(*(["up"] * (len(names) + 2)))
    if not wait_for(lambda: f"1 / {len(names)}" in V.screen(), 15): return False
    V.keys(*(["down"] * idx))
    return wait_for(lambda: f"{idx + 1} / {len(names)}" in V.screen() and any(l.startswith(">") and name in l for l in V.screen().splitlines()), 15)

def installed_dir(V, tid): return os.path.join(V.data, "installed", tid)

def test_install():
    print("== INSTALL: zip/deflate reader, Files tab, installer, install after receiving")
    port = 53371
    recv = os.path.join(ROOT, "inst", "received")
    os.makedirs(recv)
    A = make_vpk(os.path.join(recv, "a_safe.vpk"), title="Safe Game", tid="SAFE00001", ver="02.50", cid="UP0001-SAFE00001_00-0000000000000001")
    make_vpk(os.path.join(recv, "b_unsafe.vpk"), title="Unsafe Tool", tid="UNSF00001", unsafe=True)
    make_vpk(os.path.join(recv, "c_noeboot.vpk"), eboot=False)
    make_vpk(os.path.join(recv, "d_evil.vpk"), tid="EVIL00001", evil="../../escape.txt")
    make_vpk(os.path.join(recv, "e_crc.vpk"), tid="CRCX00001", method=zipfile.ZIP_STORED, corrupt=True)
    open(os.path.join(recv, "f_notzip.vpk"), "wb").write(rnd(5000, 3))
    make_vpk(os.path.join(recv, "g_self.vpk"), tid="LSND00001")
    make_vpk(os.path.join(recv, "h_many.vpk"), title="Many Files", tid="MANY00001", nfiles=120)
    S = make_vpk(os.path.join(recv, "i_stored.vpk"), title="Stored App", tid="STOR00001", method=zipfile.ZIP_STORED)
    make_vpk(os.path.join(recv, "j_nosfo.vpk"), sfo=False)
    open(os.path.join(recv, "notes.txt"), "w").write("hi")
    V = Vita("inst", port, 53399, config={"alias": "Installer", "autoAccept": 0}, env={"LS_SLOW_KEEPAWAKE_MS": "25"})
    VITAS.append(V)

    # ---- the Files tab
    V.keys("r", "r", "r")
    check("[R] x3 reaches the Files tab", V.wait_screen("tab=Files screen=files", 5), V.screen())
    check("it opens in the receive folder and lists the packages", V.has("a_safe.vpk") and V.has("b_unsafe.vpk") and V.has("VPK") and "1 / 11" in V.screen(), V.screen())
    check("a package row offers 'Install' in the hints", V.has("Install"), V.screen())
    V.keys("down", "up"); V.keys("right")
    check("[RIGHT] no longer pages the list (D-pad test)", "1 / 11" in V.screen() and "sidebar" not in V.screen(), V.screen())
    V.keys("l"); check("[L] still switches tabs", V.wait_screen("screen=history", 3), V.screen())
    V.keys("r"); V.wait_screen("screen=files", 3)
    check("the tab remembers its place", V.has("a_safe.vpk"), V.screen())

    # ---- install a normal package
    V.keys("cross")
    check("[X] on a .vpk asks before installing, with its details",
          V.wait_screen("Install this app?", 5) and "Safe Game" in V.screen() and "SAFE00001" in V.screen() and "02.50" in V.screen(), V.screen())
    check("a normal package shows no permission warning", "extended permissions" not in V.screen(), V.screen())
    V.keys("circle")
    check("[O] cancels without installing", V.wait_screen("screen=files", 3) and not os.path.exists(installed_dir(V, "SAFE00001")), V.screen())
    V.keys("cross"); V.wait_screen("Install this app?", 5); V.keys("cross")
    check("installing finishes", V.wait_screen("Installed", 20) and "Safe Game is installed" in V.screen(), V.screen())
    d = installed_dir(V, "SAFE00001")
    check("the folder handed to the system installer has every file, byte for byte",
          all(os.path.exists(os.path.join(d, n)) and open(os.path.join(d, n), "rb").read() == b for n, b in A.items()), os.listdir(d) if os.path.isdir(d) else None)
    hb = os.path.join(d, "sce_sys", "package", "head.bin")
    check("head.bin was written (1072 bytes, with the content id)", os.path.exists(hb) and os.path.getsize(hb) == 1072 and b"UP0001-SAFE00001_00-0000000000000001" in open(hb, "rb").read(), None)
    check("the scratch folder was cleaned up", not os.path.exists(os.path.join(V.data, "pkg")))
    V.keys("cross")
    check("closing returns to the Files tab", V.wait_screen("screen=files", 3), V.screen())

    # ---- extended permissions warning
    V.keys("down"); V.keys("cross")
    check("a package with extended permissions shows a warning",
          V.wait_screen("Install this app?", 5) and "extended permissions" in V.screen() and "UNSF00001" in V.screen(), V.screen())
    V.keys("circle")

    # ---- broken packages
    for fname, why, label in (("c_noeboot.vpk", "eboot.bin", "no eboot.bin"), ("f_notzip.vpk", "Not a VPK/ZIP", "not a zip"),
                              ("g_self.vpk", "cannot replace itself", "this very app"), ("j_nosfo.vpk", "param.sfo", "no param.sfo")):
        check(f"cursor on {fname}", files_row(V, fname), V.screen())
        V.keys("cross")
        check(f"{label}: refused with a reason", V.wait_screen("Cannot install this package", 5) and why in V.screen(), V.screen())
        V.keys("cross"); V.wait_screen("screen=files", 3)
    check("cursor on d_evil.vpk", files_row(V, "d_evil.vpk"), V.screen())
    V.keys("cross"); V.wait_screen("Install this app?", 5); V.keys("cross")
    check("a package with a '..' path is refused while unpacking", V.wait_screen("Installation failed", 20) and "Unsafe file name" in V.screen(), V.screen())
    check("nothing escaped from the scratch folder", not os.path.exists(os.path.join(V.data, "escape.txt")) and not os.path.exists(os.path.join(ROOT, "escape.txt")))
    check("nothing was installed", not os.path.exists(installed_dir(V, "EVIL00001")))
    V.keys("cross"); V.wait_screen("screen=files", 3)
    check("cursor on e_crc.vpk", files_row(V, "e_crc.vpk"), V.screen())
    V.keys("cross"); V.wait_screen("Install this app?", 5); V.keys("cross")
    check("a damaged file inside a package is detected (checksum)", V.wait_screen("Installation failed", 20) and ("checksum" in V.screen() or "damaged" in V.screen()), V.screen())
    check("nothing was installed from it", not os.path.exists(installed_dir(V, "CRCX00001")))
    check("scratch folder cleaned after a failure", not os.path.exists(os.path.join(V.data, "pkg")))
    V.keys("cross"); V.wait_screen("screen=files", 3)

    # ---- a stored (uncompressed) package
    check("cursor on i_stored.vpk", files_row(V, "i_stored.vpk"), V.screen())
    V.keys("cross"); V.wait_screen("Install this app?", 5); V.keys("cross")
    check("a stored (not compressed) package installs too", V.wait_screen("Stored App is installed", 20), V.screen())
    d = installed_dir(V, "STOR00001")
    check("and its files are intact", all(open(os.path.join(d, n), "rb").read() == b for n, b in S.items()))
    V.keys("cross"); V.wait_screen("screen=files", 3)

    # ---- cancel while unpacking + uploads refused while installing
    check("cursor on h_many.vpk", files_row(V, "h_many.vpk"), V.screen())
    V.keys("cross"); V.wait_screen("Install this app?", 5); V.keys("cross")
    check("progress screen shows while unpacking", V.wait_screen("Installing", 5) and "Cancel" in V.screen(), V.screen())
    st, _ = prepare(port, [file_entry("x", "x.bin", b"123")], alias="Rude")
    check("a sender is told 'busy' (409) while a package installs", st == 409, st)
    V.keys("circle")
    check("[O] cancels the installation", V.wait_screen("Cancelled", 20), V.screen())
    check("a cancelled installation leaves nothing behind", not os.path.exists(installed_dir(V, "MANY00001")) and not os.path.exists(os.path.join(V.data, "pkg")))
    V.keys("cross"); V.wait_screen("screen=files", 3)

    # ---- the system installer says no
    V.stop(); VITAS.remove(V)
    V2 = Vita("inst2", port + 1, 53399, config={"alias": "Installer2", "receiveDir": recv}, env={"LS_FAKE_PROMOTE_FAIL": "1"})
    VITAS.append(V2)
    V2.keys("r", "r", "r"); V2.wait_screen("screen=files", 5)
    V2.keys("cross"); V2.wait_screen("Install this app?", 5); V2.keys("cross")
    check("if the system installer refuses, the reason is shown", V2.wait_screen("Installation failed", 20) and "promoter failed" in V2.screen(), V2.screen())
    check("and the scratch folder is cleaned up", not os.path.exists(os.path.join(V2.data, "pkg")))
    V2.stop(); VITAS.remove(V2)

    # ---- receive a .vpk, then install it with [TRIANGLE]
    V3 = Vita("inst3", port + 2, 53399, config={"alias": "Receiver3"})
    VITAS.append(V3)
    vpk_path = os.path.join(ROOT, "sent.vpk")
    sent = make_vpk(vpk_path, title="Sent Over Wi-Fi", tid="SENT00001")
    data = open(vpk_path, "rb").read()
    t, box = prepare_async(V3.port, [file_entry("v1", "sent.vpk", data, with_sha=True)], alias="PC")
    check("a .vpk transfer request arrives", V3.wait_screen("Incoming request", 10))
    V3.keys("cross"); t.join(10)
    st, j = box["r"]
    up, _ = upload(V3.port, j["sessionId"], "v1", j["files"]["v1"], data)
    check("the .vpk is received", up == 200 and os.path.exists(os.path.join(V3.recv, "sent.vpk")), up)
    check("the result screen offers to install it with [TRIANGLE]", V3.wait_screen("Install sent.vpk", 8) and "TRI" in V3.screen().replace("[TRI]", "TRI"), V3.screen())
    V3.keys("triangle")
    check("[TRIANGLE] opens the install question for that package", V3.wait_screen("Install this app?", 5) and "Sent Over Wi-Fi" in V3.screen(), V3.screen())
    V3.keys("cross")
    check("the received package installs", V3.wait_screen("Sent Over Wi-Fi is installed", 20), V3.screen())
    check("its files are intact", all(open(os.path.join(installed_dir(V3, "SENT00001"), n), "rb").read() == b for n, b in sent.items()))
    V3.keys("cross")
    check("closing returns to where we were (Receive tab)", V3.wait_screen("This device", 5), V3.screen())

    # a plain file offers nothing; two packages send the user to the Files tab
    t, box = prepare_async(V3.port, [file_entry("t1", "plain.txt", b"hello")], alias="PC")
    V3.wait_screen("Incoming request", 10); V3.keys("cross"); t.join(10)
    st, j = box["r"]; upload(V3.port, j["sessionId"], "t1", j["files"]["t1"], b"hello")
    V3.wait_screen("1 file received", 8)
    check("a plain file does not offer an installation", "Install" not in V3.screen(), V3.screen())
    V3.keys("cross"); V3.wait_screen("This device", 5)
    f2 = [file_entry("a", "one.vpk", data), file_entry("b", "two.vpk", data)]
    t, box = prepare_async(V3.port, f2, alias="PC")
    V3.wait_screen("Incoming request", 10); V3.keys("cross"); t.join(10)
    st, j = box["r"]
    upload(V3.port, j["sessionId"], "a", j["files"]["a"], data); upload(V3.port, j["sessionId"], "b", j["files"]["b"], data)
    check("two received packages: the dialog offers to install all of them", V3.wait_screen("Install all 2 packages", 8), V3.screen())
    V3.keys("triangle")
    check("[TRIANGLE] asks once for both packages", V3.wait_screen("Install these apps?", 5) and V3.has("2 packages") and V3.has("Sent Over Wi-Fi"), V3.screen())
    V3.keys("circle")
    check("[O] cancels and goes back", V3.wait_screen("This device", 5) and not os.path.exists(os.path.join(V3.data, "pkg")), V3.screen())
    V3.keys("r", "r", "r")
    check("the Files tab lists both packages", V3.wait_screen("tab=Files screen=files", 5) and V3.has("one.vpk") and V3.has("two.vpk"), V3.screen())

    # ---- Files tab: send the highlighted file from here / navigate
    V3.keys("triangle")
    check("[TRIANGLE] in the Files tab sends the highlighted file (opens the Send tab with it selected)",
          V3.wait_screen("screen=send", 5) and "1 item selected" in V3.screen() and ("one.vpk" in V3.screen()), V3.screen())
    V3.keys("r"); V3.wait_screen("screen=history", 3); V3.keys("r"); V3.wait_screen("screen=files", 3)
    V3.keys("circle")
    check("[O] goes up a folder", wait_for(lambda: V3.has("received/") and not V3.has("one.vpk"), 5), V3.screen())
    time.sleep(0.4)
    bad = sanitizer_clean([V3])
    check("no sanitizer / crash messages in the installer test Vita", not bad, bad)
    V3.stop(); VITAS.remove(V3)


# =========================================================================== QUEUE + AUTO-INSTALL
def receive_files(V, files, alias="PC", fp=None, accept_key=None):
    """Sends {name: bytes} to the Vita (it must accept on its own, or accept_key presses a button)."""
    entries = [file_entry(f"f{i}", n, d) for i, (n, d) in enumerate(files.items())]
    t, box = prepare_async(V.port, entries, alias=alias, fp=fp)
    if accept_key:
        V.wait_screen("Incoming request", 10); V.keys(accept_key)
    t.join(15)
    st, j = box["r"]
    if st != 200: return st
    for i, (n, d) in enumerate(files.items()):
        upload(V.port, j["sessionId"], f"f{i}", j["files"][f"f{i}"], d)
    return 200

def test_queue():
    print("== QUEUE: several packages in one go, and installing received packages automatically")
    pk = os.path.join(ROOT, "qsrc"); os.makedirs(pk)
    blobs, content = {}, {}
    for name, kw in (("alpha", dict(title="Alpha App", tid="ALPH00001")), ("beta", dict(title="Beta Tool", tid="BETA00001", unsafe=True)),
                     ("gamma", dict(title="Gamma Game", tid="GAMM00001", ver="03.00")),
                     ("badcrc", dict(title="Broken Crc", tid="BCRC00001", method=zipfile.ZIP_STORED, corrupt=True)),
                     ("many", dict(title="Many Files", tid="MANY00002", nfiles=120))):
        content[name] = make_vpk(os.path.join(pk, name + ".vpk"), **kw)
        blobs[name] = open(os.path.join(pk, name + ".vpk"), "rb").read()
    notzip = rnd(4000, 7)
    V = Vita("queue", 53410, 53399, config={"alias": "Queuer", "autoAccept": 2}, env={"LS_SLOW_KEEPAWAKE_MS": "25"})
    VITAS.append(V)

    # ---- install everything that was received with one question
    check("three packages and a broken file arrive", receive_files(V, {"alpha.vpk": blobs["alpha"], "beta.vpk": blobs["beta"], "gamma.vpk": blobs["gamma"], "bad.vpk": notzip}) == 200)
    check("the result offers to install all four", V.wait_screen("Install all 4 packages", 10), V.screen())
    V.keys("triangle")
    check("one question lists the installable apps", V.wait_screen("Install these apps?", 5) and V.has("Alpha App") and V.has("Beta Tool") and V.has("Gamma Game"), V.screen())
    check("the broken package is left out, with the reason", V.has("1 package cannot be installed") and V.has("bad.vpk"), V.screen())
    check("extended permissions are flagged", V.has("extended permissions"), V.screen())
    V.keys("circle")
    check("[O] cancels: nothing is installed", V.wait_screen("This device", 5) and not os.path.exists(installed_dir(V, "ALPH00001")), V.screen())
    check("... and nothing is left in the scratch folder", not os.path.exists(os.path.join(V.data, "pkg")))

    # the Files tab for the real run (also covers ticking)
    V.keys("r", "r", "r")
    check("Files tab opens in the receive folder", V.wait_screen("tab=Files screen=files", 5) and V.has("alpha.vpk"), V.screen())
    V.keys("select"); V.wait_screen("Options", 5)
    check("the menu has no queue entry while nothing is ticked", not any(t.startswith("Install the") for t in V.menu_items()), V.menu_items())
    V.menu_pick("Tick everything")
    V.wait_screen("4 selected", 6)
    V.keys("select"); V.wait_screen("Options", 5)
    check("the menu does not repeat the install button", not any(t.startswith("Install") for t in V.menu_items()), V.menu_items())
    V.keys("circle"); V.wait_screen("screen=files", 5)
    files_cursor(V, V.recv, "alpha.vpk")
    V.keys("cross")                                       # X with packages ticked: one question for all
    check("it asks once (same list)", V.wait_screen("Install these apps?", 8) and V.has("1 package cannot be installed"), V.screen())
    c0 = V.log().count("finished chime played")
    V.keys("cross")
    check("they install one after another", V.wait_screen("All installed", 30) and V.has("3 of 3 installed"), V.screen())
    time.sleep(0.8)
    check("a queue installed by hand chimes once, at its very end", V.log().count("finished chime played") - c0 == 1, V.log().count("finished chime played") - c0)
    check("every package is installed, byte for byte", all(all(open(os.path.join(installed_dir(V, tid), n), "rb").read() == b for n, b in content[k].items())
          for k, tid in (("alpha", "ALPH00001"), ("beta", "BETA00001"), ("gamma", "GAMM00001"))), None)
    check("the summary lists each app", V.has("Alpha App") and V.has("Beta Tool") and V.has("Gamma Game"), V.screen())
    check("the scratch folder is clean", not os.path.exists(os.path.join(V.data, "pkg")))
    V.keys("cross")
    check("[X] closes the summary", V.wait_screen("screen=files", 5), V.screen())
    for tid in ("ALPH00001", "BETA00001", "GAMM00001"): shutil.rmtree(installed_dir(V, tid), ignore_errors=True)
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Untick everything"); V.wait_screen("screen=files", 5)

    # ---- a failing package does not stop the others
    for f in os.listdir(V.recv): os.remove(os.path.join(V.recv, f))
    receive_files(V, {"a.vpk": blobs["alpha"], "b.vpk": blobs["badcrc"], "c.vpk": blobs["gamma"]})
    V.keys("cross"); V.wait_screen("screen=files", 5)
    V.keys("select"); V.wait_screen("Options", 5)
    V.menu_pick("Tick everything"); V.wait_screen("3 selected", 6)
    files_cursor(V, V.recv, "a.vpk"); V.keys("cross")
    V.wait_screen("Install these apps?", 8); V.keys("cross")
    check("a broken package ends in 'Finished with problems' ...", V.wait_screen("Finished with problems", 30) and V.has("2 of 3 installed"), V.screen())
    check("... the others were installed anyway", os.path.isdir(installed_dir(V, "ALPH00001")) and os.path.isdir(installed_dir(V, "GAMM00001")) and not os.path.isdir(installed_dir(V, "BCRC00001")), None)
    check("the summary says what went wrong", V.has("Broken Crc"), V.screen())
    V.keys("cross"); V.wait_screen("screen=files", 5)
    for tid in ("ALPH00001", "GAMM00001"): shutil.rmtree(installed_dir(V, tid), ignore_errors=True)
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Untick everything"); V.wait_screen("screen=files", 5)

    # ---- cancelling stops the rest
    for f in os.listdir(V.recv): os.remove(os.path.join(V.recv, f))
    receive_files(V, {"1many.vpk": blobs["many"], "2alpha.vpk": blobs["alpha"]})
    V.keys("cross"); V.wait_screen("screen=files", 5)
    V.keys("select"); V.wait_screen("Options", 5)
    V.menu_pick("Tick everything"); V.wait_screen("2 selected", 6)
    files_cursor(V, V.recv, "1many.vpk"); V.keys("cross")
    V.wait_screen("Install these apps?", 8); V.keys("cross")
    check("progress shows which package is being installed", V.wait_screen("Installing 1 of 2", 10) and V.has("Next: Alpha App"), V.screen())
    check("a sender is told 'busy' while the queue runs", prepare(V.port, [file_entry("x", "x.bin", b"1")])[0] == 409)
    V.keys("circle")
    check("[O] cancels this package and the rest", V.wait_screen("Finished with problems", 20) and V.has("cancelled") and V.has("not installed"), V.screen())
    check("nothing was installed", not os.path.isdir(installed_dir(V, "MANY00002")) and not os.path.isdir(installed_dir(V, "ALPH00001")) and not os.path.exists(os.path.join(V.data, "pkg")), None)
    V.keys("cross"); V.wait_screen("screen=files", 5)
    V.stop(); VITAS.remove(V)

    # ---- automatic installation: off by default, everyone, favorites only
    V = Vita("autoinst", 53411, 53399, config={"alias": "Auto", "autoAccept": 2})
    VITAS.append(V)
    check("auto-install is off by default", V.config().get("autoInstall", 0) == 0 or "autoInstall" not in V.config())
    receive_files(V, {"alpha.vpk": blobs["alpha"]})
    check("off: a received package only offers to be installed", V.wait_screen("Install alpha.vpk", 8) and not os.path.isdir(installed_dir(V, "ALPH00001")), V.screen())
    V.keys("cross"); V.wait_screen("This device", 5)
    V.goto_setting("autoinst")
    check("Settings has the row, set to Off", V.wait_screen("Install received .vpk files", 5) and V.wait_screen("Off - always ask", 5), V.screen())
    V.keys("cross")
    check("[X] opens a dropdown with the three choices", V.wait_screen("Choose with the D-pad", 5) and V.has("Favorites only") and V.has("Everyone"), V.screen())
    V.keys("down", "cross")
    check("a choice has to be confirmed: what it does and its risk", V.wait_screen("Use this setting?", 5) and V.has("WHAT IT DOES") and V.has("RISK") and V.has("can run anything"), V.screen())
    V.keys("circle")
    check("[O] = no: the setting is unchanged", V.wait_screen("screen=settings", 5) and V.config().get("autoInstall", 0) == 0, V.config())
    V.choose("autoinst", 1)
    check("[X] = yes: Favorites only is saved", wait_for(lambda: V.config().get("autoInstall") == 1, 8) and V.wait_screen("Favorites only", 5), V.config())
    V.choose("autoinst", 2)
    check("Everyone (risky) is saved after the question", wait_for(lambda: V.config().get("autoInstall") == 2, 8) and V.wait_screen("risky", 5), V.config())
    V.choose("autoinst", 0)
    check("and back to Off", wait_for(lambda: V.config().get("autoInstall") == 0, 8), V.config())
    V.stop(); VITAS.remove(V)

    fav_fp = "ab" * 32
    V = Vita("autoinst2", 53412, 53399, config={"alias": "Auto2", "autoAccept": 2, "autoInstall": 1,
             "favorites": [{"fingerprint": fav_fp, "alias": "Friend"}]})
    VITAS.append(V)
    receive_files(V, {"alpha.vpk": blobs["alpha"]}, alias="Stranger", fp="cd" * 32)
    check("favorites only: a stranger's package is not installed by itself", V.wait_screen("Install alpha.vpk", 8) and not os.path.isdir(installed_dir(V, "ALPH00001")), V.screen())
    check("... so the download chimes right away", wait_for(lambda: V.log().count("finished chime played") == 1, 5), V.log().count("finished chime played"))
    V.keys("cross"); V.wait_screen("This device", 5)
    receive_files(V, {"gamma.vpk": blobs["gamma"]}, alias="Friend", fp=fav_fp)
    check("favorites only: a favorite's package installs by itself", V.wait_screen("Gamma Game is installed", 20), V.screen())
    check("it really is installed", os.path.isdir(installed_dir(V, "GAMM00001")), None)
    time.sleep(0.8)
    check("automatic install: ONE chime for download, unpacking and installing together", V.log().count("finished chime played") == 2, V.log().count("finished chime played"))
    V.keys("cross"); V.wait_screen("This device", 5)
    check("closing returns to the Receive tab", "tab=Receive" in V.screen(), V.screen())
    V.stop(); VITAS.remove(V)

    V = Vita("autoinst3", 53413, 53399, config={"alias": "Auto3", "autoAccept": 2, "autoInstall": 2})
    VITAS.append(V)
    receive_files(V, {"bad.vpk": notzip})
    check("auto: a broken package is not installed, the user is told", V.wait_screen("bad.vpk", 8) and not os.path.exists(os.path.join(V.data, "installed")) and V.wait_screen("Not a VPK/ZIP", 8), V.screen())
    check("... nothing will follow, so the download chimes", wait_for(lambda: V.log().count("finished chime played") == 1, 5), V.log().count("finished chime played"))
    V.keys("cross"); V.wait_screen("This device", 5)
    receive_files(V, {"g.vpk": blobs["gamma"], "a.vpk": blobs["alpha"], "x.vpk": notzip})
    check("auto, several: the queue starts without a question", V.wait_screen("All installed", 30) and V.has("2 of 2 installed"), V.screen())
    check("... and both are installed", os.path.isdir(installed_dir(V, "GAMM00001")) and os.path.isdir(installed_dir(V, "ALPH00001")), None)
    time.sleep(0.8)
    check("an automatic queue chimes once, at its very end", V.log().count("finished chime played") == 2, V.log().count("finished chime played"))
    V.keys("cross"); V.wait_screen("This device", 5)
    receive_files(V, {"self.vpk": make_vpk(os.path.join(pk, "self.vpk"), tid="LSND00001") and open(os.path.join(pk, "self.vpk"), "rb").read()})
    check("auto: VitaSend never replaces itself", V.wait_screen("cannot replace itself", 8) and not os.path.isdir(installed_dir(V, "LSND00001")), V.screen())
    bad = sanitizer_clean([V])
    check("no sanitizer errors", not bad, bad)
    V.stop(); VITAS.remove(V)


# =========================================================================== POLISH: battery, settings groups, sound, dropdown, screensaver
def saver_pos(V):
    m = re.search(r"screen=saver pos=(\d+),(\d+)", V.screen())
    return (int(m.group(1)), int(m.group(2))) if m else None

def test_x11():
    print("== X11: Receive focus + device details, battery/clock settings, screensaver rules, Open app, dropdown rule")
    env = {"LS_IDLE_UNIT_MS": "100", "LS_FAKE_BATTERY": "87", "LS_FAKE_CLOCK": "14:07", "LS_SLOW_KEEPAWAKE_MS": "25"}
    pk = os.path.join(ROOT, "x11src"); os.makedirs(pk)
    make_vpk(os.path.join(pk, "many.vpk"), title="Many Files", tid="MANY00002", nfiles=120)
    many = open(os.path.join(pk, "many.vpk"), "rb").read()

    # ---- hidden ids in the device details
    V = Vita("x11h", 53440, 53399, config={"alias": "Hider", "hideIds": True, "screensaver": 0})
    VITAS.append(V)
    V.wait_screen("sidebar", 8); V.keys("right", "cross")
    check("device details respect 'hide IP address and fingerprint'", V.wait_screen("screen=info", 5) and V.has("Hidden") and not re.search(r"[0-9a-f]{32}", V.screen()) and LOCAL_IP.rsplit(".", 1)[0] + "." not in V.screen(), V.screen())
    V.stop(); VITAS.remove(V)

    # ---- battery and clock in the main application
    V = Vita("x11b", 53441, 53399, config={"alias": "Clocker", "screensaver": 0}, env=env)
    VITAS.append(V)
    check("by default the battery is shown and the clock is not", V.wait_screen("87%", 8) and not V.has("14:07"), V.screen())
    V.keys("l", "l", "l", "l", "l") if False else V.keys("r", "r", "r", "r")
    V.wait_screen("tab=Settings screen=settings", 5)
    V.goto_setting("clock")
    check("Settings > Clock: Off", V.wait_screen("Clock", 5) and V.wait_screen("Off - not shown", 5), V.screen())
    V.keys("cross")
    check("[X] switches the clock on and saves it", wait_for(lambda: V.config().get("showClock") is True, 6) and V.wait_screen("14:07", 5), V.config())
    first = [l for l in V.screen().splitlines() if l.strip()][:3]
    check("the clock sits next to the battery (top right, same line group)", any("14:07" in l for l in first) and any("87%" in l for l in first), first)
    V.keys("circle"); V.wait_screen("This device", 5)
    check("the clock is on every page", V.has("14:07") and V.has("87%"), V.screen())
    V.goto_setting("battery")
    V.keys("cross")
    check("[X] on 'Battery indicator' hides the battery, the clock stays", wait_for(lambda: V.config().get("showBattery") is False, 6) and wait_for(lambda: not V.has("87%") and V.has("14:07"), 5), V.config())
    V.stop(); VITAS.remove(V)

    # ---- the screensaver: automatic installation, wake rules, Open app
    V = Vita("x11s", 53442, 53399, config={"alias": "Saver11", "autoAccept": 2, "autoInstall": 2, "screensaver": 30}, env=env)
    VITAS.append(V)
    check("the screensaver starts", V.wait_screen("screen=saver", 10), V.screen())
    receive_files(V, {"many.vpk": many})
    check("it stays on while the file arrives and is installed - showing the progress", wait_for(lambda: "screen=saver" in V.screen() and re.search(r"Installing \d+%", V.screen()), 15), V.screen()[:120])
    check("it is woken once, by the END of the installation", V.wait_screen("screen=install", 30) and V.has("Installed") and "screen=saver" not in V.screen(), V.screen()[:200])
    lg = V.log()
    check("... no wake-up for the download in between", "screensaver off (transfer finished)" not in lg and lg.count("screensaver off (installation finished)") == 1, lg[-400:])
    check("the done screen offers to open the app with Square", V.has("Open Many Files") and V.has("Open the app"), V.screen())
    ons, offs = V.log().count("screensaver on"), V.log().count("screensaver off")
    check("left alone on the 'Installed' screen, the screensaver comes back", V.wait_screen("screen=saver", 8), V.screen()[:120])
    time.sleep(1.5)
    check("... and it stays (it does not switch itself off again)", "screen=saver" in V.screen() and V.log().count("screensaver on") == ons + 1 and V.log().count("screensaver off") == offs, V.log()[-300:])
    V.tap(60, 162)
    check("touching wakes it", wait_for(lambda: "screen=saver" not in V.screen(), 4), V.screen()[:120])
    time.sleep(0.8)
    check("... and that touch does nothing else", "screen=install" in V.screen() and "tab=Receive" in V.screen(), V.screen()[:120])
    V.keys("square")
    check("[SQUARE] opens the installed app", wait_for(lambda: "launch: MANY00002" in V.log(), 5), V.log()[-300:])
    V.keys("cross")
    V.wait_screen("tab=Receive screen=receive", 5)
    check("the screensaver comes back on the Receive page, too", V.wait_screen("screen=saver", 8), V.screen()[:120])
    V.tap(60, 162)
    wait_for(lambda: "screen=saver" not in V.screen(), 4); time.sleep(0.8)
    check("a touch on a tab only wakes the screensaver, it does not switch tabs", "tab=Receive" in V.screen() and "screen=receive" in V.screen(), V.screen()[:120])
    V.wait_screen("screen=saver", 8)
    V.keys("cross")
    wait_for(lambda: "screen=saver" not in V.screen(), 4); time.sleep(0.6)
    check("a button only wakes it, it does not press anything", "screen=receive" in V.screen() and "screen=info" not in V.screen(), V.screen()[:120])
    bad = sanitizer_clean([V])
    check("no sanitizer errors", not bad, bad)
    V.stop(); VITAS.remove(V)

def test_x13():
    print("== X13: the screensaver wakes after a SLOW installation (manual and automatic)")
    env = {"LS_IDLE_UNIT_MS": "100", "LS_SLOW_PROMOTE_MS": "5000", "LS_SLOW_INPUT_MS": "12"}
    pk = os.path.join(ROOT, "x13src"); os.makedirs(pk)
    make_vpk(os.path.join(pk, "slow.vpk"), title="Slow One", tid="SLOW00001", nfiles=20)
    slow = open(os.path.join(pk, "slow.vpk"), "rb").read()
    # automatic installation, the screensaver comes on while the installer works
    V = Vita("x13a", 53450, 53399, config={"alias": "Slow13", "autoAccept": 2, "autoInstall": 2, "screensaver": 40}, env=env)
    VITAS.append(V)
    check("the screensaver starts", V.wait_screen("screen=saver", 10), V.screen())
    receive_files(V, {"slow.vpk": slow})
    check("it is still on while the slow installer works", wait_for(lambda: "screen=saver" in V.screen() and V.has("Installing"), 15), V.screen()[:120])
    check("the end of the installation wakes it", V.wait_screen("screen=install", 30) and V.has("Installed") and "screen=saver" not in V.screen(), V.screen()[:200])
    lg = V.log()
    check("... exactly once, and the log says why", lg.count("screensaver off (installation finished)") == 1 and "installation idle" in lg, lg[-400:])
    time.sleep(0.5)
    check("... and it stays awake for a while (no flapping)", "screen=saver" not in V.screen() and V.log().count("screensaver off") == 1 and V.log().count("screensaver on") == 1, V.screen()[:100] + V.log()[-300:])
    V.stop(); VITAS.remove(V)
    # manual installation from the Files tab
    V = Vita("x13m", 53451, 53399, config={"alias": "Slow13m", "autoAccept": 2, "autoInstall": 0, "screensaver": 60}, env=dict(env, LS_SLOW_PROMOTE_MS="9000"))
    VITAS.append(V)
    receive_files(V, {"slow.vpk": slow})
    V.wait_screen("screen=transfer", 10); time.sleep(0.5); V.keys("cross")
    V.wait_screen("screen=receive", 5)
    V.keys("r", "r", "r")
    V.wait_screen("tab=Files screen=files", 5)
    V.keys("cross"); V.wait_screen("Install this app?", 5); V.keys("cross")
    check("the manual installation started", V.wait_screen("Installing", 5), V.screen()[:200])
    check("the screensaver comes on during the manual installation", V.wait_screen("screen=saver", 10), V.screen()[:120])
    check("the end of the installation wakes it", wait_for(lambda: "screen=saver" not in V.screen() and V.has("Installed"), 30), V.screen()[:200] + V.log()[-500:])
    check("... once", V.log().count("screensaver off (installation finished)") == 1, V.log()[-300:])
    V.tap(60, 162)
    time.sleep(0.8)
    V.keys("cross")
    bad = sanitizer_clean([V])
    check("no sanitizer errors", not bad, bad)
    V.stop(); VITAS.remove(V)

def test_dpad():
    print("== D-PAD TEST: LEFT goes to the tab list, UP/DOWN change tab there, RIGHT / X / O go back")
    V = Vita("dpad", 53430, 53409, config={"alias": "Dpad", "screensaver": 0})
    VITAS.append(V)
    V.wait_screen("tab=Receive screen=receive", 8)
    check("it starts with the focus on the tab list (nothing on the Receive page needs steering)", V.wait_screen("tab=Receive screen=receive sidebar", 8), V.screen())
    V.keys("right")
    check("[RIGHT] enters the page: the 'This device' block is selected", wait_for(lambda: "sidebar" not in V.screen() and V.has(">This device") and V.has("Press X for details") and V.has("To receive: open LocalSend"), 4), V.screen())
    V.keys("cross")
    check("[X] opens the device details", V.wait_screen("screen=info", 4) and V.has("Fingerprint") and V.has("Receives into") and V.has("Protocol") and V.has("VitaSend v" + VERSION), V.screen())
    V.keys("circle")
    check("[O] closes them again", wait_for(lambda: "tab=Receive screen=receive" in V.screen() and "sidebar" not in V.screen(), 4), V.screen())
    V.keys("left")
    check("[LEFT] moves the focus to the tab list", V.wait_screen("sidebar", 4) and V.has("Switch tab"), V.screen())
    V.keys("down")
    check("[DOWN] in the tab list opens the next tab (Send) and keeps the focus there", V.wait_screen("tab=Send screen=send sidebar", 4), V.screen())
    V.keys("down", "down")
    check("[DOWN] x2 reaches Files", V.wait_screen("tab=Files screen=files sidebar", 4), V.screen())
    V.keys("down")
    check("[DOWN] reaches Settings", V.wait_screen("tab=Settings screen=settings sidebar", 4), V.screen())
    V.keys("down")
    check("[DOWN] on the last tab wraps to Receive", V.wait_screen("tab=Receive screen=receive sidebar", 4), V.screen())
    V.keys("up")
    check("[UP] on the first tab wraps to Settings", V.wait_screen("tab=Settings screen=settings sidebar", 4), V.screen())
    before = V.config()
    V.keys("cross")
    check("[X] in the tab list only returns to the page (changes nothing)", wait_for(lambda: "tab=Settings screen=settings" in V.screen() and "sidebar" not in V.screen(), 4) and V.config() == before, V.screen())
    V.keys("left"); V.wait_screen("sidebar", 4)
    V.keys("right")
    check("[RIGHT] returns to the page", wait_for(lambda: "sidebar" not in V.screen(), 4), V.screen())
    V.keys("left"); V.wait_screen("sidebar", 4)
    V.keys("circle")
    check("[O] in the tab list returns to the page, it does not leave the tab", wait_for(lambda: "sidebar" not in V.screen() and "tab=Settings" in V.screen(), 4), V.screen())
    V.keys("down", "down")
    check("[DOWN] on the page still moves the list", "sidebar" not in V.screen(), V.screen())
    V.keys("right")
    check("[RIGHT] on a page does nothing", "sidebar" not in V.screen() and "tab=Settings" in V.screen(), V.screen())
    V.keys("left"); V.wait_screen("sidebar", 4)
    V.keys("l")
    check("[L] still switches tabs (and gives the page the focus)", wait_for(lambda: "tab=Files screen=files" in V.screen() and "sidebar" not in V.screen(), 4), V.screen())
    V.keys("r"); V.wait_screen("tab=Settings", 4)
    V.keys("left"); V.wait_screen("sidebar", 4)
    V.tap(700, 60)
    check("a touch gives the focus back to the page", wait_for(lambda: "sidebar" not in V.screen(), 4), V.screen())
    V.keys("left"); V.wait_screen("sidebar", 4)
    V.keys("down"); V.wait_screen("tab=Receive", 4)
    V.keys("cross"); time.sleep(0.2)
    check("the Receive tab works after leaving the tab list", "sidebar" not in V.screen() and "tab=Receive screen=receive" in V.screen(), V.screen())


def test_polish():
    print("== POLISH: battery, grouped settings + About, finished sound, dropdown questions, screensaver")
    env = {"LS_IDLE_UNIT_MS": "100", "LS_FAKE_BATTERY": "87", "LS_FAKE_CLOCK": "14:07"}   # "30 seconds" of idle = 3 s in the tests
    V = Vita("polish", 53420, 53399, config={"alias": "Polish", "autoAccept": 2, "screensaver": 0}, env=env)
    VITAS.append(V)
    check("the battery percentage is shown on screen", V.wait_screen("87%", 5), V.screen())
    check("Quick Save is no longer on the Receive page", "Quick Save" not in V.screen() and "Recent activity" in V.screen(), V.screen())
    V.keys("l")
    V.wait_screen("screen=settings", 5)
    check("the battery is shown on every page", V.has("87%"), V.screen())
    check("settings are divided by headers", V.has("THIS DEVICE") and V.has("RECEIVING"), V.screen())
    V.goto_setting("exists")
    wait_for(lambda: "When a received file" in V.screen(), 5)
    scr = V.screen()
    qi, ii, ei = scr.find("Quick Save"), scr.find("Install received"), scr.find("When a received file")
    check("'Install received .vpk files' sits right under Quick Save, then 'When a received file already exists'", 0 <= qi < ii < ei, scr)
    V.goto_setting("sound")
    check("headers follow the scrolling list", wait_for(lambda: V.has("DISPLAY AND SOUND") and V.has("On - a short chime") and "THIS DEVICE" not in V.screen(), 6), V.screen())
    V.goto_setting("about")
    check("the About box shows version, date and credit", V.wait_screen("ABOUT", 5) and V.has("VitaSend  v" + VERSION) and V.has("Version date  2026-") and V.has("Created by trashman"), V.screen())
    check("... and no 'support' line while none is set", "Support" not in V.screen(), V.screen())
    check("... and a 'Check for updates' button", V.has("Check for updates"), V.screen())
    V.keys("cross")
    check("[X] on the About box looks for updates (there is no GitHub here: it fails, with a reason)", V.wait_screen("screen=update", 5) and V.wait_screen("Update check failed", 15), V.screen())
    V.keys("circle")
    check("[O] closes the update dialog", V.wait_screen("screen=settings", 5), V.screen())
    V.keys("circle"); V.wait_screen("This device", 5)

    # ---- the finished chime: on by default, switchable
    chimes = lambda: V.log().count("finished chime played")
    check("a chime has not played yet", chimes() == 0)
    check("a file arrives", receive_files(V, {"c1.txt": b"hello"}) == 200 and V.wait_screen("1 file received", 8), V.screen())
    check("receiving a file plays the chime (default: on)", wait_for(lambda: chimes() == 1, 5), V.log()[-300:])
    V.keys("cross"); V.wait_screen("This device", 5)
    V.goto_setting("sound"); V.keys("cross")
    check("[X] on 'Sound' switches it off and saves it", wait_for(lambda: V.config().get("sound") is False, 8) and V.wait_screen("Off - silent", 5), V.config())
    check("switching it off plays nothing", chimes() == 1)
    V.keys("circle"); V.wait_screen("This device", 5)
    check("a second file arrives", receive_files(V, {"c2.txt": b"hello"}) == 200 and V.wait_screen("1 file received", 8), V.screen())
    time.sleep(1.0)
    check("with sound off the chime stays silent", chimes() == 1, chimes())
    V.keys("cross"); V.wait_screen("This device", 5)
    V.goto_setting("sound"); V.keys("cross")
    check("switching it on again plays it once as a preview", wait_for(lambda: V.config().get("sound") is True and chimes() == 2, 8), (V.config(), chimes()))
    V.keys("circle"); V.wait_screen("This device", 5)
    pk = os.path.join(ROOT, "polish.vpk")
    make_vpk(pk, title="Chime App", tid="CHIM00001")
    check("a package arrives", receive_files(V, {"chime.vpk": open(pk, "rb").read()}) == 200 and V.wait_screen("Install chime.vpk", 8), V.screen())
    check("... and the chime plays for the download", wait_for(lambda: chimes() == 3, 5), chimes())
    V.keys("triangle"); V.wait_screen("Install this app?", 5); V.keys("cross")
    check("the installation finishes", V.wait_screen("Chime App is installed", 20), V.screen())
    check("installing by hand: ONE chime, when the installation is finished (none for unpacking)", wait_for(lambda: chimes() == 4, 5), chimes())
    time.sleep(0.8)
    check("... and no more than that", chimes() == 4, chimes())
    V.keys("cross"); V.wait_screen("This device", 5)

    # ---- dropdown: cancel, unchanged choice, risk text
    V.goto_setting("accept"); V.keys("cross")
    check("Quick Save opens a dropdown showing the current choice", V.wait_screen("Choose with the D-pad", 5) and V.has("Off") and V.has("Favorites") and V.has("Everyone"), V.screen())
    V.keys("circle")
    check("[O] closes the dropdown", V.wait_screen("screen=settings", 5) and V.config().get("autoAccept") == 2)
    V.keys("cross"); V.wait_screen("Choose with the D-pad", 5); V.keys("cross")
    check("choosing the value that is already set asks nothing", V.wait_screen("screen=settings", 5) and "Use this setting?" not in V.screen(), V.screen())
    V.keys("cross"); V.wait_screen("Choose with the D-pad", 5); V.keys("up", "cross")
    check("a change shows what it does and what it risks", V.wait_screen("Use this setting?", 5) and V.has("Quick Save:  Favorites") and V.has("WHAT IT DOES") and V.has("RISK") and V.has("announce a favorite"), V.screen())
    V.keys("circle")
    check("[O] = no: Quick Save stays as it was", V.wait_screen("screen=settings", 5) and V.config().get("autoAccept") == 2, V.config())
    V.stop(); VITAS.remove(V)

    # ---- screensaver
    V = Vita("saver", 53421, 53399, config={"alias": "Saver", "autoAccept": 0}, env=env)
    VITAS.append(V)
    check("the screensaver starts after 30 s idle (3 s in the test)", V.wait_screen("screen=saver", 10), V.screen())
    check("it shows the name - but no battery and no clock any more", V.has("VitaSend") and not V.has("87%") and not V.has("14:07") and "Recent activity" not in V.screen(), V.screen())
    p1 = saver_pos(V)
    time.sleep(1.2)
    p2 = saver_pos(V)
    check("the picture moves", p1 and p2 and p1 != p2, (p1, p2))
    check("... inside the screen", p2 and 0 <= p2[0] <= 960 - 372 and 0 <= p2[1] <= 544 - 118, p2)
    seen = set()
    for _ in range(40):
        seen.add(saver_pos(V)); time.sleep(0.1)
    xs = [p[0] for p in seen if p]; ys = [p[1] for p in seen if p]
    check("it travels smoothly in all directions (many positions, bounded)", len(seen) > 20 and min(xs) >= 0 and max(xs) <= 588 and min(ys) >= 0 and max(ys) <= 426, (len(seen), min(xs), max(xs)))
    V.keys("triangle")
    check("a button press wakes it ...", wait_for(lambda: "screen=receive" in V.screen(), 5), V.screen()[:80])
    time.sleep(0.5)
    check("... and does nothing else", "Announcing" not in V.screen(), V.screen())
    time.sleep(1.2)
    check("the idle timer starts again after waking", "screen=saver" not in V.screen(), V.screen()[:80])
    check("it comes back after being idle again", V.wait_screen("screen=saver", 10))
    V.tap(480, 270)
    check("a touch wakes it too", wait_for(lambda: "screen=receive" in V.screen(), 5), V.screen()[:80])
    check("it comes back again", V.wait_screen("screen=saver", 10))
    t, box = prepare_async(V.port, [file_entry("q", "wake.txt", b"w")], alias="Waker")
    check("an incoming request wakes the app", V.wait_screen("Incoming request", 10) and "screen=saver" not in V.screen(), V.screen()[:80])
    V.keys("cross"); t.join(10)
    st, j = box["r"]
    upload(V.port, j["sessionId"], "q", j["files"]["q"], b"w")
    V.wait_screen("1 file received", 8); V.keys("cross"); V.wait_screen("This device", 5)
    check("a message wakes it", V.wait_screen("screen=saver", 10))
    msg = {"info": sender_info("Texter", None), "files": {"m": {"id": "m", "fileName": "msg.txt", "size": 5, "fileType": "text/plain", "preview": "hello", "sha256": None}}}
    raw_request(V.port, "POST", API + "prepare-upload", json.dumps(msg).encode(), {"Content-Type": "application/json"})
    check("(the message arrived and brought the app back)", wait_for(lambda: "screen=saver" not in V.screen(), 8), V.screen()[:80])
    V.keys("cross"); V.keys("circle"); time.sleep(0.5)
    V.stop(); VITAS.remove(V)

    # ---- receiving goes on during the screensaver; its progress is shown; the end of the transfer wakes the app
    V = Vita("saver2", 53422, 53399, config={"alias": "Saver2", "autoAccept": 2}, env=env)
    VITAS.append(V)
    check("saver on", V.wait_screen("screen=saver", 10))
    big = rnd(600000, 77)
    st, j = prepare(V.port, [file_entry("k", "big.bin", big)], alias="Sender")
    up = {}
    def slow_upload(): up["r"] = upload(V.port, j["sessionId"], "k", j["files"]["k"], big, chunk=20000, slow=0.15)
    ut = threading.Thread(target=slow_upload, daemon=True); ut.start()
    check("a transfer does not wake it: the saver shows the percentage", wait_for(lambda: "screen=saver" in V.screen() and re.search(r"Receiving \d+%", V.screen()), 8), V.screen())
    ut.join(30)
    check("the file arrived during the screensaver", up.get("r", (0,))[0] == 200 and open(os.path.join(V.recv, "big.bin"), "rb").read() == big, up)
    check("the end of the transfer wakes the app", wait_for(lambda: "screen=saver" not in V.screen() and V.has("1 file received"), 8), V.screen()[:200])
    time.sleep(1.2)
    check("the timer was reset by the file", "screen=saver" not in V.screen(), V.screen()[:80])
    V.keys("cross"); V.wait_screen("This device", 5)
    V.goto_setting("saver")
    check("Settings has a 'Screensaver' row", V.wait_screen("Screensaver", 5) and V.wait_screen("After 30 seconds", 5), V.screen())
    V.keys("cross")
    check("[X] opens a dropdown with the 5 choices", V.wait_screen("Choose with the D-pad", 5) and V.has("After 5 minutes") and V.has("After 1 minute") and V.has("Off"), V.screen())
    V.keys("circle"); V.wait_screen("screen=settings", 5)
    for idx, want, text in ((2, 60, "After 1 minute"), (3, 120, "After 2 minutes"), (4, 300, "After 5 minutes"), (0, 0, "Off - the screen always"), (1, 30, "After 30 seconds")):
        V.pick(idx)
        check(f"dropdown: {want} s is saved at once", wait_for(lambda: V.config().get("screensaver") == want, 8) and V.wait_screen(text, 5), V.config())
    V.stop(); VITAS.remove(V)

    V = Vita("saver3", 53423, 53399, config={"alias": "Saver3", "screensaver": 0}, env=env)
    VITAS.append(V)
    time.sleep(4.5)
    check("with the screensaver set to Off it never starts", "screen=saver" not in V.screen(), V.screen()[:80])
    bad = sanitizer_clean([V])
    check("no sanitizer errors", not bad, bad)
    V.stop(); VITAS.remove(V)

# =========================================================================== PRIVACY: hide IP address and fingerprint
def test_privacy():
    print("== PRIVACY: hide IP address and fingerprint")
    V = Vita("priv", 53381, 53399, config={"alias": "Privacy"})
    VITAS.append(V)
    fp = V.fp
    check("by default the address and fingerprint are shown", V.has(LOCAL_IP) and len(fp) == 64 and fp in V.screen(), V.screen())
    V.goto_setting("hide")
    check("settings list has the 'Hide IP address and fingerprint' row", V.wait_screen("Hide IP address and fingerprint", 5) and V.wait_screen("Off - shown", 5), V.screen())
    V.keys("cross")
    check("[X] turns it on and saves it", wait_for(lambda: V.config().get("hideIds") is True, 8) and V.wait_screen("On - not shown", 5), V.config())
    check("the left menu no longer shows the address", V.wait_screen("Address hidden", 3) is not None and LOCAL_IP not in V.screen(), V.screen())
    V.keys("circle")
    check("Receive page: address and fingerprint are hidden", V.wait_screen("This device", 5) and wait_for(lambda: "Hidden (Settings" in V.screen(), 5), V.screen())
    check("neither the IP nor the fingerprint appears anywhere on screen", LOCAL_IP not in V.screen() and fp not in V.screen(), V.screen())
    V.stop(); VITAS.remove(V)
    V2 = Vita("priv2", 53382, 53399, config={"alias": "Privacy2", "hideIds": True})
    VITAS.append(V2)
    check("the setting is read from config.json at start", V2.has("Address hidden") and LOCAL_IP not in V2.screen() and "Hidden (Settings" in V2.screen(), V2.screen())
    V2.goto_setting("hide"); V2.keys("cross")
    check("[X] again turns it off (saved)", wait_for(lambda: V2.config().get("hideIds") is False, 8), V2.config())
    V2.keys("circle")
    check("and the address is shown again on the Receive page", wait_for(lambda: LOCAL_IP in V2.screen() and "Address hidden" not in V2.screen(), 8), V2.screen())
    V2.stop(); VITAS.remove(V2)

# =========================================================================== THEMES (Dark / OLED)
def ppm_pixel(path, x, y):
    d = open(path, "rb").read()
    parts = d.split(b"\n", 3)                     # "P6", "960 544", "255", data
    w, h = map(int, parts[1].split())
    i = (y * w + x) * 3
    return tuple(parts[3][i:i + 3])

def test_theme():
    print("== THEME: dark and OLED colours")
    V = Vita("theme", 53383, 53399, config={"alias": "Colours"}, extra=["--dump-ppm", os.path.join(ROOT, "theme.ppm")])
    VITAS.append(V)
    ppm = os.path.join(ROOT, "theme.ppm")
    check("default theme is the dark blue-grey one", wait_for(lambda: os.path.exists(ppm) and ppm_pixel(ppm, 946, 300) == (15, 19, 23), 6), os.path.exists(ppm) and ppm_pixel(ppm, 946, 300))
    check("sidebar is a little darker in the dark theme", wait_for(lambda: ppm_pixel(ppm, 180, 400) == (11, 14, 17), 5), ppm_pixel(ppm, 180, 400))
    V.goto_setting("theme")
    check("settings has a 'Colours' row showing Dark", V.wait_screen("Colours", 5) and V.wait_screen("Dark - the original", 5), V.screen())
    V.keys("cross")
    check("[X] switches to OLED and saves it", wait_for(lambda: V.config().get("theme") == 1, 8) and V.wait_screen("OLED - pure black", 5), V.config())
    check("the background is now pure black", wait_for(lambda: ppm_pixel(ppm, 946, 300) == (0, 0, 0) and ppm_pixel(ppm, 180, 400) == (0, 0, 0), 6), (ppm_pixel(ppm, 946, 300), ppm_pixel(ppm, 180, 400)))
    check("thin divider lines separate the black areas", ppm_pixel(ppm, 211, 300) != (0, 0, 0) and ppm_pixel(ppm, 400, 500) != (0, 0, 0), (ppm_pixel(ppm, 211, 300), ppm_pixel(ppm, 400, 500)))
    V.keys("circle")
    check("the Receive page is black too", V.wait_screen("This device", 5) and wait_for(lambda: ppm_pixel(ppm, 946, 300) == (0, 0, 0), 5))
    V.stop(); VITAS.remove(V)
    V2 = Vita("theme2", 53384, 53399, config={"alias": "Colours2", "theme": 1}, extra=["--dump-ppm", os.path.join(ROOT, "theme2.ppm")])
    VITAS.append(V2)
    check("the OLED theme is read from config.json at start", wait_for(lambda: os.path.exists(os.path.join(ROOT, "theme2.ppm")) and ppm_pixel(os.path.join(ROOT, "theme2.ppm"), 946, 300) == (0, 0, 0), 6))
    V2.goto_setting("theme"); V2.keys("cross")
    check("[X] switches back to dark", wait_for(lambda: V2.config().get("theme") == 0, 8) and wait_for(lambda: ppm_pixel(os.path.join(ROOT, "theme2.ppm"), 946, 300) == (15, 19, 23), 6))
    V2.stop(); VITAS.remove(V2)


def test_touch():
    print("== TOUCH: the front touch screen (simulated fingers)")
    BTN = dict(START=1, SELECT=2, TRI=4, O=8, X=16, SQ=32)
    recv = os.path.join(ROOT, "touch", "received")
    os.makedirs(os.path.join(recv, "adir"))
    open(os.path.join(recv, "adir", "a1.txt"), "w").write("1")
    open(os.path.join(recv, "adir", "a2.txt"), "w").write("2")
    for i in range(1, 15): open(os.path.join(recv, f"f{i:02d}.bin"), "wb").write(rnd(100 + i, i))
    src = os.path.join(ROOT, "touchsrc"); os.makedirs(src)
    SA, SB = rnd(3000, 81), rnd(4100, 82)
    open(os.path.join(src, "a-note.bin"), "wb").write(SA)
    open(os.path.join(src, "b-data.bin"), "wb").write(SB)
    R = Receiver(53392)
    V = Vita("touch", 53391, 53392, config={"alias": "Toucher", "autoAccept": 0}, browse=src)
    VITAS.append(V)
    announce_to(V, R.info)
    def tab(name): return wait_for(lambda: f"tab={name} " in V.screen(), 6)

    # ---- sidebar tabs
    for i, name in enumerate(["Receive", "Send", "History", "Files", "Settings"]):
        V.tap(100, 112 + 50 * i)
        check(f"tapping '{name}' in the sidebar opens it", tab(name), V.screen()[:60])
    V.tap(100, 112)
    tab("Receive")
    V.tap(100, 112)                                     # tapping the tab you are on does nothing bad
    time.sleep(0.4)
    check("tapping the current tab changes nothing", "tab=Receive " in V.screen())

    # ---- settings rows
    V.tap(100, 312); tab("Settings")
    check("the Settings list has group headers", V.has("THIS DEVICE") and V.has("RECEIVING"), V.screen())
    V.tap(500, 230)                                     # 3rd row: Hide IP address and fingerprint
    check("tapping a settings row changes it (Hide IP -> On)", wait_for(lambda: V.config().get("hideIds") is True, 6) and V.wait_screen("On - not shown", 4), V.config())
    V.tap(500, 230)
    check("tapping it again changes it back", wait_for(lambda: V.config().get("hideIds") is False, 6), V.config())

    # ---- on-screen keyboard by touch
    V.tap(500, 135)                                     # first row: Device name
    check("tapping 'Device name' opens the keyboard", V.wait_screen("screen=keyboard", 5), V.screen()[:60])
    for ch_row, ch_col in ((3, 6), (3, 5)):             # keyboard row 3 = "zxcvbnm,.?!@#$": z at col 0 ... 'm' col 6, 'n' col 5
        r = V.find_hit("C", (ch_row << 8) | ch_col)
        if r: V.tap(r[0] + r[2] // 2, r[1] + r[3] // 2)
    check("tapping keys types letters", V.wait_screen("Touchermn", 6), V.screen())
    tabs = [h for h in V.hits() if h[0] == "C" and int(h[2]) < 80 and int(h[5]) == 2]      # the third alphabet tab (Cyrillic)
    check("the keyboard has tappable alphabet tabs", len(tabs) == 1, V.hits()[:5])
    if tabs: V.tap(int(tabs[0][1]) + int(tabs[0][3]) // 2, int(tabs[0][2]) + int(tabs[0][4]) // 2)
    time.sleep(0.4)
    check("tapping an alphabet tab does not break the keyboard", "screen=keyboard" in V.screen())
    V.tap_hit("K", BTN["SQ"], True)
    time.sleep(0.5)
    V.tap_hit("K", BTN["SQ"], True)
    check("the hint-bar 'Delete' removes letters", wait_for(lambda: "Toucher" in V.screen() and "Toucherm" not in V.screen(), 6), V.screen())
    V.tap_hit("K", BTN["START"], True)
    check("the hint-bar 'Done' accepts the name", V.wait_screen("screen=settings", 6) and wait_for(lambda: V.config().get("alias") == "Toucher", 6), (V.screen()[:60], V.config()))

    # ---- Files tab: rows, folders, Up, scrolling
    V.tap(100, 262); tab("Files")
    check("Files tab lists the folder (15 entries)", V.wait_screen("1 / 15", 6), V.screen())
    V.tap(500, 147)                                     # first row: adir/
    check("tapping a folder opens it", V.wait_screen("a1.txt", 6) and "f01.bin" not in V.screen(), V.screen())
    up = V.find_hit("K", BTN["O"], False)
    check("a folder shows an 'Up' button", up is not None, V.hits()[:6])
    V.tap_hit("K", BTN["O"], False)
    check("tapping 'Up' goes back", V.wait_screen("f01.bin", 6) and "a1.txt" not in V.screen() and V.wait_screen("1 / 15", 4), V.screen())
    V.drag(500, 420, 500, 250)                          # finger moves up 170 px = 4 rows
    ok = wait_for(lambda: "5 / 15" in V.screen() and "f01.bin" not in V.screen() and any(l.startswith(">") and "f04.bin" in l for l in V.screen().splitlines()), 8)
    check("dragging up scrolls the list by whole rows", ok, V.screen())
    V.drag(500, 250, 500, 420)
    check("dragging down scrolls back", wait_for(lambda: "adir" in V.screen() and "f01.bin" in V.screen(), 8), V.screen())
    check("the scrollbar/selection stayed consistent", "5 / 15" in V.screen() or "1 / 15" in V.screen(), V.screen())
    V.tap(500, 185)                                     # row 2 (f01.bin): not a picture / text / .vpk -> a tap says it cannot be opened
    check("tapping a plain file opens it - here: says it cannot be opened (nothing ticked)", V.wait_screen("cannot be opened here", 6) and "selected" not in V.screen(), V.screen())
    r = [h for h in V.hits() if h[0] == "C" and int(h[3]) == 56 and int(h[5]) == 1]       # the tick circle of that row
    check("each file row has a tick circle area", len(r) == 1, V.hits()[:12])
    if r: V.tap(int(r[0][1]) + 20, int(r[0][2]) + int(r[0][4]) // 2)
    check("tapping the tick circle ticks the row", V.wait_screen("1 selected", 6) and V.wait_screen("[x]  f01.bin", 6), V.screen())
    if r: V.tap(int(r[0][1]) + 20, int(r[0][2]) + int(r[0][4]) // 2)
    check("tapping the tick circle unticks the row", wait_for(lambda: "[x]  f01.bin" not in V.screen() and "1 selected" not in V.screen(), 6), V.screen())
    if r: V.tap(int(r[0][1]) + 20, int(r[0][2]) + int(r[0][4]) // 2)
    check("tapping it again ticks it", V.wait_screen("1 selected", 6) and V.wait_screen("[x]  f01.bin", 6), V.screen())
    check("the Files page has a Menu button", V.tap_hit("K", BTN["SELECT"], False))
    check("tapping [Menu] opens the options", V.wait_screen("Options", 6) and "Untick everything (1)" in V.screen(), V.screen())
    rows = [h for h in V.hits() if h[0] == "R" and int(h[3]) == 520]
    check("each menu entry is a tap row", len(rows) == len(V.menu_items()) and len(rows) >= 5, (len(rows), V.menu_items(), V.screen()))
    unt = rows[V.menu_index("Untick everything")]
    V.tap(int(unt[1]) + 100, int(unt[2]) + 20)
    check("tapping 'Untick everything' unticks", V.wait_screen("screen=files", 5) and wait_for(lambda: "[x]" not in V.screen(), 5), V.screen())
    V.tap(100, 112); tab("Receive")

    # ---- an incoming request: buttons, hint bar, taps outside the dialog
    f1 = rnd(5000, 91)
    files = [file_entry("t-1", "touch.bin", f1)]
    t, box = prepare_async(V.port, files, alias="Tap Rejecter")
    check("request dialog appears", V.wait_screen("Incoming request", 10), V.screen())
    V.tap(50, 60)                                       # outside the card: ignored
    time.sleep(0.5)
    check("tapping outside the dialog does nothing", "Incoming request" in V.screen())
    V.tap(100, 112)                                     # the sidebar is dimmed behind the dialog: no tab switch either
    time.sleep(0.5)
    check("the dimmed sidebar cannot be tapped", "Incoming request" in V.screen())
    check("Reject has an on-screen button", V.find_hit("K", BTN["O"], False) is not None, V.hits())
    V.tap_hit("K", BTN["O"], False)
    t.join(10)
    check("tapping [Reject] rejects (403)", box.get("r", (None,))[0] == 403, box)
    check("back on the Receive page", V.wait_screen("This device", 5))

    t, box = prepare_async(V.port, files, alias="Tap Accepter")
    V.wait_screen("Incoming request", 10)
    check("Accept can be tapped in the hint bar too", V.tap_hit("K", BTN["X"], True))
    t.join(10)
    st, j = box["r"]
    check("tapping [Accept] accepts (200)", st == 200 and j and j.get("sessionId"), box)
    check("transfer dialog opens", V.wait_screen("Receiving", 5), V.screen())
    check("an unfinished transfer offers Cancel and Hide buttons", wait_for(lambda: V.find_hit("K", BTN["O"], False) and V.find_hit("K", BTN["TRI"], False), 6), V.hits())
    V.tap_hit("K", BTN["TRI"], False)                   # Hide
    check("tapping [Hide] returns to the page, the transfer keeps running", V.wait_screen("This device", 5))
    sid, tok = j["sessionId"], j["files"]
    st, _ = upload(V.port, sid, "t-1", tok["t-1"], f1)
    check("the upload completes", st == 200, st)
    check("result dialog appears", V.wait_screen("1 file received", 8), V.screen())
    V.tap_hit("K", BTN["X"], False)
    check("tapping [Close] closes it", V.wait_screen("This device", 5) and "1 file received" not in V.screen())

    # ---- a long text message scrolls under the finger
    body = "\n".join(f"Line {i:02d} of the message" for i in range(1, 21))
    t, box = prepare_async(V.port, [dict(file_entry("m", "message.txt", body.encode(), ftype="text/plain"), preview=body)], alias="Long Texter")
    V.wait_screen("Incoming request", 10)
    V.tap_hit("K", BTN["X"], False)
    t.join(10)
    check("the message viewer opens", V.wait_screen("Message from Long Texter", 8) and "Line 01 of" in V.screen() and "Line 12 of" not in V.screen(), V.screen())
    V.drag(480, 400, 480, 313)                          # 87 px up = 3 lines (29 px each)
    check("dragging up scrolls the text by 3 lines", wait_for(lambda: "Line 04 of" in V.screen() and "Line 03 of" not in V.screen() and "Line 12 of" in V.screen(), 8), V.screen())
    V.drag(480, 300, 480, 600)                          # dragging down far enough shows the start again
    check("dragging down scrolls back to the start", wait_for(lambda: "Line 01 of" in V.screen(), 8), V.screen())
    V.tap_hit("K", BTN["X"], True)
    check("the hint bar closes the viewer", V.wait_screen("This device", 5))

    # ---- sending with touch only: choose files, Done, tap the device
    V.tap(100, 162); tab("Send")
    check("the other device is listed", V.wait_screen("Fake Phone", 8), V.screen())
    check("the 'Files' button is tappable", V.tap_hit("K", BTN["SQ"], False))
    check("the file browser opens", V.wait_screen("Choose files or folders to send", 5), V.screen())
    V.tap(500, 147)
    check("tapping a file selects it", V.wait_screen("1 selected", 6), V.screen())
    V.tap(500, 185)
    check("tapping another one selects it too", V.wait_screen("2 selected", 6), V.screen())
    V.tap(500, 147)
    check("tapping a selected file again unselects it", V.wait_screen("1 selected", 6), V.screen())
    V.tap(500, 147)
    V.wait_screen("2 selected", 6)
    check("the hint bar has a 'Done' item", V.tap_hit("K", BTN["TRI"], True))
    check("back on Send with the selection", V.wait_screen("2 items selected", 6) and V.wait_screen("Fake Phone", 6), V.screen())
    check("the device row is highlighted", any(l.startswith(">") and "Fake Phone" in l for l in V.screen().splitlines()), V.screen())
    V.tap(500, 255)
    check("one tap on the device sends", V.wait_screen("Sent 2 files to Fake Phone", 20), V.screen())
    ids = {v["fileName"]: k for k, v in R.prepares[0][0]["files"].items()} if R.prepares else {}
    check("both files arrived intact", R.uploads.get(ids.get("a-note.bin")) == SA and R.uploads.get(ids.get("b-data.bin")) == SB, {k: len(v) for k, v in R.uploads.items()})
    V.tap_hit("K", BTN["X"], False)
    check("tapping [Close] returns to Send", V.wait_screen("screen=send", 5))

    # ---- the star on a device row marks a favorite
    stars = [h for h in V.hits() if h[0] == "C" and int(h[3]) == 56]
    check("each device row has a star area", len(stars) >= 1, V.hits())
    if stars:
        h = stars[-1]
        V.tap(int(h[1]) + int(h[3]) // 2, int(h[2]) + int(h[4]) // 2)
        check("tapping the star makes it a favorite", wait_for(lambda: V.config().get("favorites") or V.has("is now a favorite"), 6), V.screen())

    # ---- long lists in the browser scroll under the finger too, and the History page is tappable
    V.tap(100, 212); tab("History")
    check("History opens by tap", V.wait_screen("History", 4))
    V.tap(500, 120)
    time.sleep(0.4)
    check("tapping a history entry is harmless", "tab=History " in V.screen())

    check("no sanitizer errors during the touch tests", not sanitizer_clean([V]), sanitizer_clean([V]))
    V.stop(); VITAS.remove(V)
    R.stop()

def test_migrate():
    print("== MIGRATE: settings from the old LocalSendVita folder name")
    V = Vita("migrate", 53393, 53399, config={"alias": "Mover", "receiveDir": "ux0:data/LocalSendVita/received/sub"})
    VITAS.append(V)
    V.goto_setting("folder")
    check("a saved receive folder in the old app folder now points to VitaSend", V.wait_screen("ux0:data/VitaSend/received/sub", 6) and "LocalSendVita" not in V.screen(), V.screen())
    V.stop(); VITAS.remove(V)
    V2 = Vita("migrate2", 53394, 53399, config={"alias": "Keeper", "receiveDir": "ux0:data/Other/LocalSendVita"})
    VITAS.append(V2)
    V2.goto_setting("folder")
    check("other folders are left alone", V2.wait_screen("ux0:data/Other/LocalSendVita", 6), V2.screen())
    V2.stop(); VITAS.remove(V2)


def sorted_entries(d):
    """The folder listing in the order the program shows it: folders first, then by name (case-insensitive)."""
    return sorted(os.listdir(d), key=lambda n: (not os.path.isdir(os.path.join(d, n)), n.lower()))

def files_cursor(V, d, name):
    """Moves the Files cursor onto `name` (works for any number of entries)."""
    names = sorted_entries(d)
    idx = names.index(name)
    V.keys(*(["up"] * (len(names) + 2)))
    if not wait_for(lambda: any(l.startswith(">") and names[0] in l for l in V.screen().splitlines()), 15): return False
    V.keys(*(["down"] * idx))
    return wait_for(lambda: any(l.startswith(">") and name in l for l in V.screen().splitlines()), 15)

def test_files():
    print("== FILES: ticking, sending several items, options menu, rename, delete, new folder, free space")
    recv = os.path.join(ROOT, "files", "received")
    os.makedirs(os.path.join(recv, "docs")); os.makedirs(os.path.join(recv, "empty"))
    open(os.path.join(recv, "docs", "readme.txt"), "w").write("read me")
    A, B = rnd(1000, 101), rnd(2000, 102)
    open(os.path.join(recv, "a.bin"), "wb").write(A)
    open(os.path.join(recv, "b.bin"), "wb").write(B)
    open(os.path.join(recv, "c.txt"), "w").write("see")
    R = Receiver(53401)
    V = Vita("files", 53400, 53401, config={"alias": "Filer"})
    VITAS.append(V)
    announce_to(V, R.info)
    V.keys("r", "r", "r")
    check("Files tab opens on the received folder", V.wait_screen("tab=Files screen=files", 6) and "docs/" in V.screen() and "a.bin" in V.screen(), V.screen())
    check("the free space of the card is shown", any(" free" in l for l in V.screen().splitlines()[:4]), V.screen())
    check("every entry has an empty tick box", V.screen().count("[ ]") == 5, V.screen())

    # ---- tick two files and send them together
    files_cursor(V, recv, "a.bin")
    V.keys("square", "down", "square")
    check("[SQUARE] ticks the highlighted entry", V.wait_screen("2 selected", 6) and "[x]  a.bin" in V.screen() and "[x]  b.bin" in V.screen(), V.screen())
    V.keys("triangle")
    check("[TRIANGLE] sends the ticked items: Send tab shows them", V.wait_screen("screen=send", 5) and V.wait_screen("2 items selected", 5) and V.wait_screen("Fake Phone", 8), V.screen())
    V.keys("cross")
    check("both files arrive", V.wait_screen("Sent 2 files to Fake Phone", 20), V.screen())
    ids = {v["fileName"]: k for k, v in R.prepares[0][0]["files"].items()} if R.prepares else {}
    check("the contents are intact", R.uploads.get(ids.get("a.bin")) == A and R.uploads.get(ids.get("b.bin")) == B, {k: len(v) for k, v in R.uploads.items()})
    V.finish()
    V.keys("r", "r", "r")
    check("after sending, nothing stays ticked", V.wait_screen("tab=Files screen=files", 5) and "[x]" not in V.screen() and "selected" not in V.screen(), V.screen())

    # ---- nothing ticked: [TRIANGLE] sends the highlighted entry (a folder works too)
    files_cursor(V, recv, "docs")
    V.keys("triangle")
    check("with nothing ticked [TRIANGLE] takes the highlighted entry", V.wait_screen("screen=send", 5) and V.wait_screen("1 item selected", 5) and "docs" in V.screen(), V.screen())
    R.reset()
    V.keys("cross")
    check("a folder goes with its contents", V.wait_screen("Sent 1 file to Fake Phone", 20), V.screen())
    check("the file inside has its relative path", R.prepares and [v["fileName"] for v in R.prepares[0][0]["files"].values()] == ["docs/readme.txt"], R.prepares)
    V.finish()
    V.keys("r", "r", "r"); V.wait_screen("tab=Files screen=files", 5)

    # ---- options menu: tick all / untick
    V.keys("select")
    check("[SELECT] opens the options menu", V.wait_screen("Options", 5) and "Rename docs" in V.screen() and "New folder here" in V.screen(), V.screen())
    V.menu_pick("Tick everything")
    check("'Tick everything in this folder' ticks all 5", V.wait_screen("5 selected", 6) and V.screen().count("[x]") == 5, V.screen())
    V.keys("select")
    V.wait_screen("Options", 5)
    check("with ticked items the menu offers untick / copy / move / delete them (Send is a button)", all(any(t.startswith(p) for t in V.menu_items()) for p in ("Untick everything (5)", "Copy the 5 ticked", "Move the 5 ticked", "Delete the 5 ticked")) and not any(t.startswith("Send") for t in V.menu_items()), (V.menu_items(), V.screen()))
    V.menu_pick("Untick everything")
    check("'Untick everything' clears the ticks", wait_for(lambda: "[x]" not in V.screen() and "screen=files" in V.screen(), 6), V.screen())

    # ---- new folder
    V.keys("select"); V.wait_screen("Options", 5)
    V.menu_pick("New folder here")
    check("'New folder here' asks for a name (pre-filled)", V.wait_screen("Name of the new folder", 5) and "New folder" in V.screen(), V.screen())
    V.keys(*(["square"] * 10), *osk_keys("pics"), "start")
    check("the folder is created and selected", wait_for(lambda: os.path.isdir(os.path.join(recv, "pics")), 8) and V.wait_screen("pics/", 5), V.screen())
    V.keys("select"); V.wait_screen("Options", 5)
    V.menu_pick("New folder here")
    V.wait_screen("Name of the new folder", 5)
    V.keys(*osk_keys_unicode("\b" * 10 + "pics"), "start")
    check("a name that exists is refused and the keyboard stays", V.wait_screen("exists already", 6) and "screen=keyboard" in V.screen(), V.screen())
    V.keys("circle"); V.wait_screen("screen=files", 5)
    V.keys("select"); V.wait_screen("Options", 5)
    V.menu_pick("New folder here")
    V.wait_screen("Name of the new folder", 5)
    V.keys(*osk_keys_unicode("\b" * 10 + "a/b"), "start")
    check("a name with / is refused", V.wait_screen("That name cannot be used", 6) and "screen=keyboard" in V.screen(), V.screen())
    V.keys("circle")
    check("cancelling goes back", V.wait_screen("screen=files", 5))

    # ---- rename
    files_cursor(V, recv, "c.txt")
    V.keys("select"); V.wait_screen("Options", 5)
    V.menu_pick("Rename c.txt")
    check("rename opens the keyboard with the current name", V.wait_screen("screen=keyboard", 5) and "c.txt" in V.screen(), V.screen())
    V.keys(*osk_keys_unicode("\b" * 5 + "a.bin"), "start")
    check("renaming onto an existing name is refused", V.wait_screen("exists already", 6) and "screen=keyboard" in V.screen(), V.screen())
    V.keys("circle"); V.wait_screen("screen=files", 5)
    V.keys("select"); V.wait_screen("Options", 5)
    V.menu_pick("Rename c.txt")
    V.wait_screen("screen=keyboard", 5)
    V.keys(*osk_keys_unicode("\b" * 5 + "z.txt"), "start")
    check("rename works on disk", wait_for(lambda: os.path.exists(os.path.join(recv, "z.txt")) and not os.path.exists(os.path.join(recv, "c.txt")), 8), os.listdir(recv))
    check("the renamed entry is shown and highlighted", wait_for(lambda: any(l.startswith(">") and "z.txt" in l for l in V.screen().splitlines()), 6), V.screen())

    # ---- delete: No keeps it, Yes removes it
    V.keys("select"); V.wait_screen("Options", 5)
    V.menu_pick("Delete z.txt")
    check("delete asks first", V.wait_screen("Delete?", 5) and 'Delete "z.txt"?' in V.screen(), V.screen())
    V.keys("circle")
    time.sleep(0.5)
    check("[O] keeps the file", os.path.exists(os.path.join(recv, "z.txt")) and V.wait_screen("screen=files", 5))
    V.keys("select"); V.wait_screen("Options", 5)
    V.menu_pick("Delete z.txt")
    V.wait_screen("Delete?", 5)
    V.keys("cross")
    check("[X] deletes it", wait_for(lambda: not os.path.exists(os.path.join(recv, "z.txt")), 8) and V.wait_screen("Deleted 1 item", 6), os.listdir(recv))

    # ---- delete a folder with everything inside, and several ticked items at once
    files_cursor(V, recv, "docs")
    V.keys("select"); V.wait_screen("Options", 5)
    V.menu_pick("Delete docs")
    V.wait_screen("Delete?", 5); V.keys("cross")
    check("a folder is deleted together with its contents", wait_for(lambda: not os.path.exists(os.path.join(recv, "docs")), 8), os.listdir(recv))
    files_cursor(V, recv, "a.bin")
    V.keys("square", "down", "square")
    V.wait_screen("2 selected", 5)
    V.keys("select"); V.wait_screen("Options", 5)
    V.menu_pick("Delete the 2 ticked")
    check("the question counts the ticked items", V.wait_screen("Delete these 2 items?", 5), V.screen())
    V.keys("cross")
    check("all ticked items are deleted", wait_for(lambda: not os.path.exists(os.path.join(recv, "a.bin")) and not os.path.exists(os.path.join(recv, "b.bin")), 8) and V.wait_screen("Deleted 2 items", 6), os.listdir(recv))
    check("they are no longer ticked", "[x]" not in V.screen() and not any("selected" in l for l in V.screen().splitlines()[:4]), V.screen())

    # ---- the app's own data folder cannot be deleted
    V.keys("circle")
    V.wait_screen("data/", 5)
    check("[O] goes up one folder", "received/" in V.screen() and "data/" in V.screen(), V.screen())
    files_cursor(V, V.dir, "data")
    V.keys("select"); V.wait_screen("Options", 5)
    V.menu_pick("Delete data")
    check("VitaSend's own data folder is protected", V.wait_screen("contains VitaSend's own data", 6) and os.path.exists(os.path.join(V.data, "config.json")), V.screen())
    check("no sanitizer errors during the Files tests", not sanitizer_clean([V]), sanitizer_clean([V]))
    V.stop(); VITAS.remove(V)
    R.stop()

def test_exists():
    print("== EXISTS: what happens to a received file whose name is taken")
    # ---- Skip: only new files get a token; everything known -> 204
    recv = os.path.join(ROOT, "exists", "received"); os.makedirs(recv)
    open(os.path.join(recv, "dup.bin"), "wb").write(b"OLD")
    V = Vita("exists", 53402, 53399, config={"alias": "Skipper", "onExists": 1, "autoAccept": 2})
    VITAS.append(V)
    new, fresh = rnd(500, 111), rnd(700, 112)
    st, j = prepare(V.port, [file_entry("d", "dup.bin", new), file_entry("n", "fresh.bin", fresh)], alias="PC")
    check("Skip: only the new file gets a token", st == 200 and set(j["files"]) == {"n"}, (st, j))
    up, _ = upload(V.port, j["sessionId"], "n", j["files"]["n"], fresh)
    check("the new file arrives, the old one is untouched", up == 200 and open(os.path.join(recv, "fresh.bin"), "rb").read() == fresh
          and open(os.path.join(recv, "dup.bin"), "rb").read() == b"OLD" and not os.path.exists(os.path.join(recv, "dup (1).bin")), os.listdir(recv))
    check("the result says what was skipped", V.wait_screen("1 file received, 1 skipped", 8), V.screen())
    V.keys("cross"); V.wait_screen("This device", 5)
    st, j = prepare(V.port, [file_entry("d", "dup.bin", new)], alias="PC")
    check("Skip: when everything exists the answer is 204 (nothing to send)", st == 204, (st, j))
    check("the Vita says nothing new arrived", V.wait_screen("already there", 6), V.screen())
    check("history notes the skipped request", wait_for(lambda: any("skipped" in h.get("summary", "") for h in V.history()), 5), V.history())
    V.keys("cross"); V.wait_screen("This device", 5)
    V.goto_setting("exists")
    check("Settings: the row shows the current choice", V.wait_screen("When a received file already exists", 5) and V.wait_screen("Skip - ignore", 5), V.screen())
    V.keys("cross")
    check("[X] opens a dropdown list (3 options)", V.wait_screen("Choose with the D-pad", 5) and V.has("Keep both") and V.has("Skip") and V.has("Overwrite") and "Use this setting?" not in V.screen(), V.screen())
    V.keys("down", "cross")
    check("choosing Overwrite saves it at once (no question)", wait_for(lambda: V.config().get("onExists") == 2, 8) and V.wait_screen("Overwrite - replace", 5), V.config())
    V.pick(0)
    check("choosing Keep both saves it", wait_for(lambda: V.config().get("onExists") == 0, 8) and V.wait_screen("Keep both", 5), V.config())
    V.stop(); VITAS.remove(V)

    # ---- Overwrite: the dialog warns, the old file is replaced
    recv2 = os.path.join(ROOT, "exists2", "received"); os.makedirs(recv2)
    open(os.path.join(recv2, "dup.bin"), "wb").write(b"OLD")
    V2 = Vita("exists2", 53405, 53399, config={"alias": "Overwriter", "onExists": 2})
    VITAS.append(V2)
    t, box = prepare_async(V2.port, [file_entry("d", "dup.bin", new)], alias="PC")
    check("Overwrite: the request dialog warns", V2.wait_screen("Incoming request", 10) and V2.wait_screen("1 already exists - will be OVERWRITTEN", 5), V2.screen())
    V2.keys("cross"); t.join(10)
    st, j = box["r"]
    up, _ = upload(V2.port, j["sessionId"], "d", j["files"]["d"], new)
    check("the old file is replaced, no copy is made", up == 200 and open(os.path.join(recv2, "dup.bin"), "rb").read() == new and os.listdir(recv2) == ["dup.bin"], os.listdir(recv2))
    V2.stop(); VITAS.remove(V2)

    # ---- Keep both (the default): the new file gets " (1)"
    recv3 = os.path.join(ROOT, "exists3", "received"); os.makedirs(recv3)
    open(os.path.join(recv3, "dup.bin"), "wb").write(b"OLD")
    V3 = Vita("exists3", 53406, 53399, config={"alias": "Keeper"})
    VITAS.append(V3)
    t, box = prepare_async(V3.port, [file_entry("d", "dup.bin", new)], alias="PC")
    check("Keep both: the dialog says so", V3.wait_screen("Incoming request", 10) and V3.wait_screen("1 already exists - kept, new ones get (1)", 5), V3.screen())
    V3.keys("cross"); t.join(10)
    st, j = box["r"]
    up, _ = upload(V3.port, j["sessionId"], "d", j["files"]["d"], new)
    check("both files exist afterwards", up == 200 and open(os.path.join(recv3, "dup.bin"), "rb").read() == b"OLD"
          and open(os.path.join(recv3, "dup (1).bin"), "rb").read() == new, os.listdir(recv3))
    check("no sanitizer errors", not sanitizer_clean([V3]), sanitizer_clean([V3]))
    V3.stop(); VITAS.remove(V3)

def test_retry():
    print("== RETRY: a failed send can be repeated with the files that are left")
    src = os.path.join(ROOT, "retrysrc"); os.makedirs(src)
    A, B, C = rnd(3000, 121), rnd(4000, 122), rnd(5000, 123)
    for n, d in (("a1.bin", A), ("b2.bin", B), ("c3.bin", C)): open(os.path.join(src, n), "wb").write(d)
    R = Receiver(53404)
    V = Vita("retry", 53403, 53404, config={"alias": "Retrier"}, browse=src)
    VITAS.append(V)
    announce_to(V, R.info)
    V.to_browser()
    V.keys("cross", "down", "cross", "down", "cross")
    check("three files are selected", V.wait_screen("3 selected", 6), V.screen())
    V.keys("triangle")
    V.wait_screen("screen=send", 5); V.wait_screen("Fake Phone", 8)
    R.fail_ids = {"f1"}                                 # the second file (b2.bin) cannot be stored
    V.keys("cross")
    check("the send fails on the second file", V.wait_screen("Failed on b2.bin", 20), V.screen())
    check("the first file did arrive", R.uploads.get("f0") == A, list(R.uploads))
    check("the dialog offers a retry with the number of files left", V.wait_screen("Retry (2 files left)", 5) and "TRI" in V.screen().replace("[TRI]", "TRI"), V.screen())
    R.fail_ids = set(); R.uploads.clear()
    V.keys("triangle")
    check("[TRIANGLE] retries", V.wait_screen("Sent 2 files to Fake Phone", 20), V.screen())
    names = sorted(v["fileName"] for v in R.prepares[1][0]["files"].values()) if len(R.prepares) > 1 else []
    check("only the files that did not arrive are sent again", names == ["b2.bin", "c3.bin"], names)
    ids = {v["fileName"]: k for k, v in R.prepares[1][0]["files"].items()}
    check("their contents are intact", R.uploads.get(ids["b2.bin"]) == B and R.uploads.get(ids["c3.bin"]) == C, {k: len(v) for k, v in R.uploads.items()})
    check("after a good send there is nothing to retry", "Retry" not in V.screen(), V.screen())
    check("history has the failed and the repeated send", wait_for(lambda: [h["status"] for h in V.history() if h["out"]][-2:] in ([1, 0], [2, 0], [3, 0]) or len([h for h in V.history() if h["out"]]) >= 2, 5), V.history())
    V.finish()

    # ---- a refused send can be retried too; a cancelled text message as well
    R.reset(); R.mode = "reject"
    V.to_browser()
    V.keys("cross", "triangle"); V.wait_screen("screen=send", 5); V.wait_screen("Fake Phone", 8)
    V.keys("cross")
    check("a refused send fails clearly", V.wait_screen("the other device said no", 15), V.screen())
    check("... and offers a retry", V.wait_screen("Retry (1 left)", 5), V.screen())
    R.mode = "accept"; R.uploads.clear()
    V.keys("triangle")
    check("the retry goes through once the other side accepts", V.wait_screen("Sent 1 file to Fake Phone", 20), V.screen())
    V.finish()
    R.reset(); R.mode = "busy"
    V.keys("r"); V.wait_screen("screen=send", 5)
    V.keys("triangle"); V.wait_screen("Message to send", 5); V.keys(*osk_keys("hi"), "start")
    V.wait_screen("Text message", 10); V.wait_screen("Fake Phone", 8)
    V.keys("cross")
    check("a failed text message offers a retry", V.wait_screen("busy with another transfer", 15) and V.wait_screen("Retry (1 left)", 5), V.screen())
    R.mode = "text204"
    V.keys("triangle")
    check("the text message is delivered by the retry", V.wait_screen("Message delivered to Fake Phone", 15), V.screen())
    check("no sanitizer errors", not sanitizer_clean([V]), sanitizer_clean([V]))
    V.stop(); VITAS.remove(V)
    R.stop()


# =========================================================================== SCREENSHOTS (not part of the test run)
def test_shots():
    """Renders the pictures for docs/screens/ with the very same drawing code the Vita runs:
         python3 host/test_transfer.py --only shots [--shots-dir DIR]
       The battery shows 69 %, the fake devices have made-up names, the addresses are documentation addresses."""
    from PIL import Image
    global sender_info
    out = os.path.abspath(args.shots_dir); os.makedirs(out, exist_ok=True)
    orig_info = sender_info
    sender_info = lambda alias="Test Sender", fp=None: dict(orig_info(alias, fp), deviceModel="Windows")
    env = {"LS_FAKE_BATTERY": "69", "LS_SLOW_PROMOTE_MS": "7000"}
    print("== SCREENSHOTS ->", out)

    def snap(V, name, wait=0.9):
        time.sleep(wait + 0.3)                                   # (the program writes the picture at most every 250 ms)
        Image.open(os.path.join(V.dir, "screen.ppm")).save(os.path.join(out, name + ".png"), optimize=True)
        print("  ", name)

    def new_vita(name, port, config, browse=None, extra_env=None):
        V = Vita(name, port, 53352, config=config, browse=browse, env=dict(env, **(extra_env or {})),
                 extra=["--dump-ppm", os.path.join(ROOT, name, "screen.ppm")])
        VITAS.append(V); os.makedirs(V.recv, exist_ok=True)
        return V

    # Paths are relative to a scratch folder, so the pictures show Vita-style names ("ux0:data/VitaSend/received") - a colon is
    # a fine character in a Linux folder name.
    cwd = os.path.join(ROOT, "shots-cwd"); os.makedirs(cwd); os.chdir(cwd)
    RECV, PICS = "ux0:data/VitaSend/received", "ux0:picture"
    os.makedirs(os.path.join(PICS, "Backups"))
    for n, sz in (("IMG_2041.jpg", 2_400_000), ("IMG_2042.jpg", 3_100_000), ("notes.txt", 2_100), ("slides.pdf", 842_000)):
        open(os.path.join(PICS, n), "wb").write(rnd(sz, 5))
    peers = []
    for port, alias, model, typ in ((53352, "Clever Pear", "Windows", "desktop"), (53353, "Sunny Fig", "Pixel 8", "mobile"), (53354, "Quiet Mango", "macOS", "desktop")):
        r = Receiver(port, alias=alias); r.info.update(deviceModel=model, deviceType=typ); peers.append(r)

    V = new_vita("shots", 53317, {"alias": "Vita-7F3A", "screensaver": 0, "autoAccept": 0, "receiveDir": RECV}, browse=PICS)
    V.recv = os.path.join(cwd, RECV); os.makedirs(V.recv, exist_ok=True)
    make_vpk(os.path.join(V.recv, "SampleGame-1.20.vpk"), title="Sample Game", tid="SMPL00001", ver="01.20")
    make_vpk(os.path.join(V.recv, "SystemTool.vpk"), title="System Tool", tid="SYST00001", ver="00.90", unsafe=True)
    # ---- an incoming request, a running transfer, the result
    fl = [file_entry("a", "holiday-photo.jpg", rnd(3_100_000, 1)), file_entry("b", "report.pdf", rnd(842_000, 2)), file_entry("c", "notes.txt", rnd(2_100, 3))]
    t, box = prepare_async(V.port, fl, alias="Clever Pear")
    V.wait_screen("Incoming request", 10); snap(V, "request")
    V.keys("cross"); t.join(15)
    j = box["r"][1]
    datas = {"a": rnd(3_100_000, 1), "b": rnd(842_000, 2), "c": rnd(2_100, 3)}
    th = threading.Thread(target=lambda: [upload(V.port, j["sessionId"], i, j["files"][i], datas[i], chunk=40000, slow=0.045) for i in "abc"], daemon=True)
    th.start(); snap(V, "transfer", 1.6); th.join(30)
    V.wait_screen("received", 10); snap(V, "transfer-done", 0.5)
    V.keys("cross"); V.wait_screen("screen=receive", 5)
    # ---- a text message
    t, box = prepare_async(V.port, [dict(file_entry("m", "message.txt", b"x", ftype="text/plain"), preview="Dinner at 7 - bring the charger!")], alias="Sunny Fig")
    V.wait_screen("Incoming request", 10); V.keys("cross"); t.join(10)
    V.wait_screen("Dinner at 7", 10); snap(V, "message", 0.4)
    V.keys("circle"); V.wait_screen("screen=receive", 5)
    time.sleep(1.0); V.keys("left"); time.sleep(0.3); V.keys("right")
    snap(V, "receive", 0.5)
    V.keys("cross"); V.wait_screen("screen=info", 5); snap(V, "device-info", 0.4)
    V.keys("circle"); V.wait_screen("screen=receive", 5)
    # ---- History, Files, installer
    def expect(V, text, what, t=8):
        if not V.wait_screen(text, t): print("   !! expected", repr(text), "for", what, "- screen is:", V.screen()[:160].replace("\n", " | "))
    def tab(V, name):
        for _ in range(6):
            if V.screen().startswith(f"@ tab={name} ") and "Choose" not in V.screen()[:60]: return
            V.keys("r"); time.sleep(0.45)
        print("   !! could not reach tab", name)
    tab(V, "Files"); snap(V, "files", 0.5)
    def to_row(V, name):                      # (files_row() waits for an "n / m" counter, which short lists do not show)
        names = sorted(os.listdir(V.recv), key=str.lower)
        V.keys(*(["up"] * (len(names) + 2))); time.sleep(0.6)
        V.keys(*(["down"] * names.index(name)))
        if not wait_for(lambda: any(l.startswith(">") and name in l for l in V.screen().splitlines()), 8): print("   !! cursor not on", name)
    for vpk, shot in (("SampleGame-1.20.vpk", "install-ask"), ("SystemTool.vpk", "install-ask-unsafe")):
        to_row(V, vpk); V.keys("cross"); expect(V, "Install this app?", shot); snap(V, shot, 0.4)
        V.keys("circle"); expect(V, "screen=files", "back to Files")
    to_row(V, "SampleGame-1.20.vpk"); V.keys("cross"); expect(V, "Install this app?", "install"); V.keys("cross")
    expect(V, "Installing", "install progress", 5); snap(V, "install-progress", 2.2)
    expect(V, "is installed", "install done", 25); snap(V, "install-done", 0.5)
    V.keys("circle"); expect(V, "screen=files", "back to Files")
    # ---- Send: choose files, devices, send (slowly, to catch the progress), type a message
    for r in peers: announce_to(V, r.info)
    tab(V, "Send")
    V.keys("square"); expect(V, "Choose files or folders to send", "browser")
    V.keys("down", "cross", "down", "cross"); time.sleep(0.5)
    snap(V, "browser", 0.4)
    V.keys("triangle"); expect(V, "screen=send", "Send tab"); wait_for(lambda: V.has("Quiet Mango"), 8)
    V.keys("down"); snap(V, "send", 0.4)
    peers[2].mode = "slow"; peers[2].slow_delay = 0.03
    V.keys("down", "cross"); expect(V, "Sending", "sending", 8); snap(V, "sending", 0.9)
    expect(V, "Sent 2 files", "sent", 20); V.keys("cross"); expect(V, "screen=send", "Send tab"); time.sleep(0.5)
    V.keys("triangle"); expect(V, "screen=keyboard", "keyboard")
    V.type_text("Hello from my Vita"); expect(V, "Hello from my Vita", "typed text", 40); snap(V, "keyboard", 0.5)
    V.keys("circle"); time.sleep(0.6)
    tab(V, "History"); snap(V, "history", 0.5)
    # ---- Settings
    tab(V, "Settings"); expect(V, "screen=settings", "Settings")
    V.goto_setting("name"); snap(V, "settings", 0.5)
    V.goto_setting("sound"); expect(V, "short chime", "Display and sound rows", 6); snap(V, "settings-display", 0.6)
    V.goto_setting("saver"); V.keys("cross"); expect(V, "Choose with the D-pad", "dropdown"); snap(V, "dropdown-screensaver", 0.5)
    V.keys("circle"); time.sleep(0.6)
    V.goto_setting("accept"); V.keys("cross"); expect(V, "Choose with the D-pad", "dropdown"); V.keys("down", "down", "cross")
    expect(V, "Use this setting?", "confirm"); snap(V, "setting-confirm", 0.4)
    V.keys("circle"); time.sleep(0.6)
    if "Choose with the D-pad" in V.screen(): V.keys("circle"); time.sleep(0.6)
    V.goto_setting("about"); time.sleep(0.6); expect(V, "VitaSend v", "About box", 5); snap(V, "settings-about", 0.5)
    V.stop(); VITAS.remove(V)
    # ---- moving files: the options menu, a move waiting for its folder, the copy between two cards
    V = new_vita("shots-move", 53321, {"alias": "Vita-7F3A", "screensaver": 0, "receiveDir": "ux0:data/Documents"},
                 extra_env={"LS_FAIL_RENAME": "1", "LS_SLOW_MOVE_MS": "450"})
    D = "ux0:data/Documents"
    os.makedirs(os.path.join(D, "Archive")); os.makedirs(os.path.join(D, "Notes"))
    for n, sz in (("holiday-photo.jpg", 2_600_000), ("notes.txt", 2_100), ("report.pdf", 842_000)):
        open(os.path.join(D, n), "wb").write(rnd(sz, 6))
    V.keys("r", "r", "r"); expect(V, "tab=Files screen=files", "files tab (move)", 6)
    files_cursor(V, D, "holiday-photo.jpg"); V.keys("square", "down", "down", "square"); expect(V, "2 selected", "ticked", 5)
    V.keys("select"); expect(V, "Options", "options menu", 5); snap(V, "files-menu", 0.6)
    V.menu_pick("Move the 2 ticked"); expect(V, "Moving 2 items", "move marked", 5)
    files_cursor(V, D, "Archive"); V.keys("cross"); expect(V, "Move here", "destination open", 5); time.sleep(2.5); snap(V, "files-move", 0.3)
    V.keys("triangle"); expect(V, "screen=move", "copy dialog", 6); snap(V, "move-progress", 0.4)
    expect(V, "Moved 2 items", "move done", 30)
    V.stop(); VITAS.remove(V)
    # ---- delete with progress, unzip
    V = new_vita("shots-ops", 53323, {"alias": "Vita-7F3A", "screensaver": 0, "receiveDir": "ux0:data/Backups"},
                 extra_env={"LS_SLOW_DELETE_MS": "60", "LS_SLOW_UNZIP_MS": "300"})
    B = "ux0:data/Backups"
    for d in range(6):
        os.makedirs(os.path.join(B, "old-saves", f"slot{d}"))
        for f in range(12): open(os.path.join(B, "old-saves", f"slot{d}", f"save{f}.dat"), "wb").write(b"x" * 400)
    with zipfile.ZipFile(os.path.join(B, "homebrew-pack.zip"), "w", zipfile.ZIP_STORED) as z:
        for i in range(8): z.writestr(f"pack/tool{i}.bin", rnd(260000, 70 + i))
    V.keys("r", "r", "r"); expect(V, "tab=Files screen=files", "files tab (ops)", 6)
    files_cursor(V, B, "homebrew-pack.zip"); V.keys("cross"); expect(V, "Unzip this file?", "unzip question", 6); snap(V, "unzip-ask", 0.4)
    V.keys("cross"); expect(V, "Unzipping", "unzip running", 6); time.sleep(1.2); snap(V, "unzip-progress", 0.3)
    expect(V, "Unzipped", "unzip done", 30)
    V.keys("cross"); V.wait_screen("screen=files", 5)
    files_cursor(V, B, "old-saves"); V.keys("select"); expect(V, "Options", "options (delete)", 5)
    V.menu_pick("Delete old-saves"); expect(V, "Delete?", "delete question", 5); V.keys("cross")
    expect(V, "screen=delete", "delete running", 5); time.sleep(1.5); snap(V, "delete-progress", 0.3)
    expect(V, "Deleted 1 item", "delete done", 30)
    V.stop(); VITAS.remove(V)
    # ---- the viewer: a picture, a text, a queue of several
    from PIL import Image as _I, ImageDraw
    P = "ux0:data/Photos"; os.makedirs(P, exist_ok=True)
    for k, (c1, c2) in enumerate((((250, 160, 60), (40, 30, 90)), ((60, 170, 200), (10, 40, 60)), ((120, 200, 120), (20, 60, 40)))):
        im = _I.new("RGB", (1600, 1200)); d = ImageDraw.Draw(im)
        for y in range(1200):
            f = y / 1199; d.line([(0, y), (1600, y)], fill=tuple(int(c1[i] + (c2[i] - c1[i]) * f) for i in range(3)))
        d.ellipse((1000 - 90 * k, 200, 1300 - 90 * k, 500), fill=(255, 240, 200))
        d.polygon([(0, 1200), (500, 700 + 60 * k), (900, 1000), (1300, 650), (1600, 1050 - 40 * k), (1600, 1200)], fill=(20, 25, 35))
        im.save(os.path.join(P, "IMG_%d.jpg" % (3100 + k)), quality=88)
    open(os.path.join(P, "readme.txt"), "w").write("VitaSend - notes\n\n" + "\n".join(
        "%02d. Pictures, text files and packages open with X. SQUARE ticks, and X then opens everything ticked as a queue you can page through with L and R." % n for n in range(1, 40)))
    V = new_vita("shots-view", 53323, {"alias": "Vita-7F3A", "screensaver": 0, "receiveDir": P})
    V.keys("r", "r", "r"); expect(V, "tab=Files screen=files", "files tab (view)", 6)
    files_cursor(V, P, "IMG_3100.jpg"); V.keys("square", "down", "square", "down", "square"); expect(V, "3 selected", "ticked pictures", 5)
    files_cursor(V, P, "IMG_3100.jpg"); expect(V, "View ticked", "hint", 4); snap(V, "files-ticked", 0.5)
    V.keys("cross"); expect(V, "screen=view", "viewer", 5); expect(V, "1600 x 1200", "picture", 8); snap(V, "view-image", 0.6)
    V.keys("circle"); expect(V, "screen=files", "back"); V.keys("select"); expect(V, "Options", "menu", 4); V.menu_pick("Untick everything"); expect(V, "screen=files", "unticked")
    files_cursor(V, P, "readme.txt"); V.keys("cross"); expect(V, "Pictures, text files", "text", 8); snap(V, "view-text", 0.6)
    V.stop(); VITAS.remove(V)
    # ---- Wi-Fi switched off: the question at the start, and the Receive page when it drops later
    wf = os.path.join(ROOT, "shots-wifi.wifi"); open(wf, "w").write("off\n")
    V = Vita("shots-wifi", 53322, 53352, config={"alias": "Vita-7F3A", "screensaver": 0}, ready=False,
             env=dict(env, LS_WIFI_FILE=wf, LS_WIFI_TURNON="nojoin", LS_WIFI_TIMEOUT_MS="7000"),
             extra=["--dump-ppm", os.path.join(ROOT, "shots-wifi", "screen.ppm")])
    VITAS.append(V)
    expect(V, "Wi-Fi is turned off", "wifi question", 8); snap(V, "wifi-ask", 0.4)
    V.keys("cross"); expect(V, "Turning on Wi-Fi...", "wifi connecting", 5); snap(V, "wifi-connecting", 0.5)
    expect(V, "Wi-Fi could not be turned on", "wifi fallback", 12); snap(V, "wifi-failed", 0.4)
    open(wf, "w").write("on\n"); expect(V, "This device", "app after wifi", 8)
    time.sleep(0.8); V.keys("right")
    open(wf, "w").write("off\n"); expect(V, "Wi-Fi not connected", "wifi dropped", 6); snap(V, "wifi-lost", 0.5)
    V.keys("triangle"); expect(V, "Turning on Wi-Fi...", "wifi connecting in app", 4); snap(V, "wifi-lost-connecting", 0.5)
    V.stop(); VITAS.remove(V)
    # ---- the OLED theme
    V = new_vita("shots-oled", 53318, {"alias": "Vita-7F3A", "screensaver": 0, "theme": 1, "autoAccept": 2, "receiveDir": RECV}, browse=PICS)
    receive_files(V, {"holiday-photo.jpg": rnd(1_200_000, 4)}, alias="Clever Pear")
    V.wait_screen("screen=transfer", 10); time.sleep(0.6); V.keys("cross"); V.wait_screen("screen=receive", 5)
    snap(V, "receive-oled", 1.0)
    V.keys("l"); V.wait_screen("screen=settings", 5); snap(V, "settings-oled", 0.5)
    V.stop(); VITAS.remove(V)
    # ---- the screensaver
    V = new_vita("shots-saver", 53319, {"alias": "Vita-7F3A", "screensaver": 30}, extra_env={"LS_IDLE_UNIT_MS": "100"})
    V.wait_screen("screen=saver", 10); time.sleep(4)
    snap(V, "screensaver", 0.3)
    V.stop(); VITAS.remove(V)
    for r in peers: r.stop()


def tree_of(d):
    """{relative path: sha256 of the content, or None for a folder} for everything below d."""
    out = {}
    for root, dirs, files in os.walk(d):
        for n in dirs: out[os.path.relpath(os.path.join(root, n), d) + "/"] = None
        for n in files: out[os.path.relpath(os.path.join(root, n), d)] = sha(open(os.path.join(root, n), "rb").read())
    return out

def test_move():
    print("== MOVE / COPY: to another folder (rename) and to another card (copy, then remove the original); copy keeps the original")
    BTN = dict(START=1, SELECT=2, TRI=4, O=8, X=16, SQ=32)
    recv = os.path.join(ROOT, "move", "received")
    os.makedirs(os.path.join(recv, "dest")); os.makedirs(os.path.join(recv, "src", "inner"))
    A, B = rnd(5000, 201), rnd(7000, 202)
    open(os.path.join(recv, "a.bin"), "wb").write(A)
    open(os.path.join(recv, "b.bin"), "wb").write(B)
    open(os.path.join(recv, "c.txt"), "w").write("see")
    open(os.path.join(recv, "src", "f1.bin"), "wb").write(rnd(300, 203))
    open(os.path.join(recv, "src", "inner", "f2.bin"), "wb").write(rnd(900, 204))
    dest = os.path.join(recv, "dest")
    V = Vita("move", 53620, 53621, config={"alias": "Mover", "screensaver": 0})
    VITAS.append(V)
    V.keys("r", "r", "r")
    check("Files tab opens", V.wait_screen("tab=Files screen=files", 6) and "dest/" in V.screen(), V.screen())

    def open_folder(d, name):
        files_cursor(V, d, name); V.keys("cross")

    # ---- one file into a folder
    files_cursor(V, recv, "a.bin")
    V.keys("select"); V.wait_screen("Options", 5)
    check("the options menu offers Move for the highlighted entry", any(t.startswith("Move a.bin") for t in V.menu_items()), V.menu_items())
    V.menu_pick("Move a.bin")
    check("'Move' marks the item and says what to do next", V.wait_screen("Moving a.bin", 5) and V.wait_screen("Moving 1 item", 5), V.screen())
    check("the header offers [Move here] for touch", V.find_hit("K", BTN["TRI"], False) is not None and "Move here" in V.screen(), V.hits()[:12])
    check("a marked row is still listed", "a.bin" in V.screen(), V.screen())
    open_folder(recv, "dest")
    check("inside the destination the hint bar says Move here", V.wait_screen("Move here", 5) and "dest" in V.screen()[:200], V.screen())
    V.keys("triangle")
    check("[TRIANGLE] moves the file here", wait_for(lambda: os.path.exists(os.path.join(dest, "a.bin")) and not os.path.exists(os.path.join(recv, "a.bin")), 8), os.listdir(recv))
    check("... with its content intact", open(os.path.join(dest, "a.bin"), "rb").read() == A)
    check("... and says so, and the marking is over", V.wait_screen("Moved 1 item", 5) and "Moving" not in V.screen().split("Moved 1 item")[0][-200:], V.screen())
    V.keys("circle"); V.wait_screen("dest/", 5)

    # ---- two ticked items at once
    files_cursor(V, recv, "b.bin")
    V.keys("square", "down", "square")
    check("two items ticked", V.wait_screen("2 selected", 5), V.screen())
    V.keys("select"); V.wait_screen("Options", 5)
    V.menu_pick("Move the 2 ticked")
    check("Move uses the ticked items and unticks them", V.wait_screen("Moving 2 items", 5) and "[x]" not in V.screen(), V.screen())
    open_folder(recv, "dest")
    V.wait_screen("Move here", 5)
    V.keys("triangle")
    check("both items arrive", wait_for(lambda: all(os.path.exists(os.path.join(dest, n)) for n in ("b.bin", "c.txt")) and not os.path.exists(os.path.join(recv, "b.bin")), 8), os.listdir(dest))
    check("... b.bin intact", open(os.path.join(dest, "b.bin"), "rb").read() == B)
    V.wait_screen("Moved 2 items", 5)
    V.keys("circle"); V.wait_screen("dest/", 5)

    # ---- a name that exists is never overwritten
    open(os.path.join(recv, "c.txt"), "w").write("a different c")
    V.keys("r"); time.sleep(0.4); V.keys("l"); V.wait_screen("tab=Files screen=files", 5)       # (the folder is listed again when the tab is opened)
    files_cursor(V, recv, "c.txt")
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Move c.txt")
    V.wait_screen("Moving c.txt", 5)
    open_folder(recv, "dest"); V.wait_screen("Move here", 5)
    V.keys("triangle")
    check("an existing name is reported", V.wait_screen("c.txt exists already", 6), V.screen())
    check("... and nothing was overwritten or lost", open(os.path.join(dest, "c.txt")).read() == "see" and open(os.path.join(recv, "c.txt")).read() == "a different c")
    V.keys("circle"); V.wait_screen("dest/", 5)

    # ---- a folder cannot go into itself; a file cannot be moved into the folder it is in
    files_cursor(V, recv, "src")
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Move src")
    V.wait_screen("Moving src", 5)
    V.keys("cross"); V.wait_screen("f1.bin", 5)
    V.keys("triangle")
    check("a folder cannot be moved into itself", V.wait_screen("cannot go into itself", 6) and os.path.exists(os.path.join(recv, "src", "f1.bin")), V.screen())
    files_cursor(V, os.path.join(recv, "src"), "f1.bin")
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Move f1.bin")
    V.wait_screen("Moving f1.bin", 5)
    V.keys("triangle")
    check("moving into the folder it is already in does nothing", V.wait_screen("already in this folder", 6) and os.path.exists(os.path.join(recv, "src", "f1.bin")), V.screen())

    # ---- cancel from the menu
    files_cursor(V, os.path.join(recv, "src"), "f1.bin")
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Move f1.bin")
    V.wait_screen("Moving f1.bin", 5)
    V.keys("select"); V.wait_screen("Options", 5)
    items = V.menu_items()
    check("with a move pending the menu offers Move here and Cancel", items[:2] == ["Move here (1 item)", "Cancel the move"], items)
    V.menu_pick("Cancel the move")
    check("cancelling the move forgets it", V.wait_screen("Move cancelled", 5) and "Moving 1 item" not in V.screen(), V.screen())

    # ---- a whole folder with sub-folders
    V.keys("circle"); V.wait_screen("dest/", 5)
    before = tree_of(os.path.join(recv, "src"))
    files_cursor(V, recv, "src")
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Move src")
    V.wait_screen("Moving src", 5)
    open_folder(recv, "dest"); V.wait_screen("Move here", 5)
    V.keys("triangle")
    check("a folder moves with everything in it", wait_for(lambda: not os.path.exists(os.path.join(recv, "src")) and tree_of(os.path.join(dest, "src")) == before, 8), (before, tree_of(dest)))
    V.stop(); VITAS.remove(V)

    # ---- touch: the [Move here] button of the header
    recv_t = os.path.join(ROOT, "move-touch", "received")
    os.makedirs(os.path.join(recv_t, "box"))
    open(os.path.join(recv_t, "t.bin"), "wb").write(rnd(100, 205))
    V = Vita("move-touch", 53624, 53625, config={"alias": "Toucher", "screensaver": 0})
    VITAS.append(V)
    V.keys("r", "r", "r"); V.wait_screen("tab=Files screen=files", 6)
    files_cursor(V, recv_t, "t.bin")
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Move t.bin")
    V.wait_screen("Moving t.bin", 5)
    files_cursor(V, recv_t, "box"); V.keys("cross"); V.wait_screen("Move here", 5)
    check("the header has a touch button for Move here", V.find_hit("K", BTN["TRI"], False) is not None, V.hits()[:12])
    V.tap_hit("K", BTN["TRI"], False)
    check("tapping it moves the file", wait_for(lambda: os.path.exists(os.path.join(recv_t, "box", "t.bin")), 8), os.listdir(recv_t))
    V.stop(); VITAS.remove(V)

    # ---- COPY: the original always stays; a name that exists gets "(1)"
    recv_d = os.path.join(ROOT, "move-dup", "received")
    os.makedirs(os.path.join(recv_d, "dest")); os.makedirs(os.path.join(recv_d, "tree", "sub"))
    CA = rnd(4000, 211)
    open(os.path.join(recv_d, "a.bin"), "wb").write(CA)
    open(os.path.join(recv_d, "c.txt"), "w").write("source c")
    open(os.path.join(recv_d, "dest", "c.txt"), "w").write("dest c")
    open(os.path.join(recv_d, "tree", "t1.bin"), "wb").write(rnd(150_000, 212))
    open(os.path.join(recv_d, "tree", "sub", "t2.bin"), "wb").write(rnd(1234, 213))
    dd = os.path.join(recv_d, "dest")
    V = Vita("move-dup", 53630, 53631, config={"alias": "Duplicator", "screensaver": 0})
    VITAS.append(V)
    V.keys("r", "r", "r"); V.wait_screen("tab=Files screen=files", 6)
    files_cursor(V, recv_d, "a.bin")
    V.keys("select"); V.wait_screen("Options", 5)
    check("the options menu offers Copy as well as Move", any(t.startswith("Copy a.bin") for t in V.menu_items()) and any(t.startswith("Move a.bin") for t in V.menu_items()), V.menu_items())
    V.menu_pick("Copy a.bin")
    check("'Copy' marks the item", V.wait_screen("Copying a.bin", 5) and V.wait_screen("Copying 1 item", 5), V.screen())
    open_d = lambda name: (files_cursor(V, recv_d, name), V.keys("cross"))
    open_d("dest"); V.wait_screen("Copy here", 5)
    check("the header button and the hint bar say Copy here", V.find_hit("K", BTN["TRI"], False) is not None and V.find_hit("K", BTN["TRI"], True) is not None, V.hits()[:12])
    V.keys("triangle")
    check("the copy arrives and the original stays", wait_for(lambda: os.path.exists(os.path.join(dd, "a.bin")) and os.path.exists(os.path.join(recv_d, "a.bin")), 8), (os.listdir(dd), os.listdir(recv_d)))
    check("... with its content intact", open(os.path.join(dd, "a.bin"), "rb").read() == CA)
    check("... and says so", V.wait_screen("Copied 1 item", 6), V.screen())
    V.keys("circle"); V.wait_screen("dest/", 5)

    files_cursor(V, recv_d, "c.txt")
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Copy c.txt")
    V.wait_screen("Copying c.txt", 5)
    open_d("dest"); V.wait_screen("Copy here", 5)
    V.keys("triangle")
    check("a name that exists gets a new one: c (1).txt", wait_for(lambda: os.path.exists(os.path.join(dd, "c (1).txt")), 8) and open(os.path.join(dd, "c (1).txt")).read() == "source c", os.listdir(dd))
    check("... and the file that was there is untouched", open(os.path.join(dd, "c.txt")).read() == "dest c")
    V.wait_screen("Copied 1 item", 8)
    V.keys("circle"); V.wait_screen("dest/", 5)

    files_cursor(V, recv_d, "a.bin")
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Copy a.bin")
    V.wait_screen("Copying a.bin", 5)
    V.keys("select"); V.wait_screen("Options", 5)
    items = V.menu_items()
    check("with a copy pending the menu says Copy here / Cancel the copy", items[:2] == ["Copy here (1 item)", "Cancel the copy"], items)
    V.menu_pick("Copy here")
    check("copying into the same folder makes 'a (1).bin'", wait_for(lambda: os.path.exists(os.path.join(recv_d, "a (1).bin")) and open(os.path.join(recv_d, "a (1).bin"), "rb").read() == CA, 8), os.listdir(recv_d))
    V.wait_screen("Copied 1 item", 8)

    before = tree_of(os.path.join(recv_d, "tree"))
    files_cursor(V, recv_d, "tree")
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Copy tree")
    V.wait_screen("Copying tree", 5)
    open_d("dest"); V.wait_screen("Copy here", 5)
    V.keys("triangle")
    check("a folder is copied with everything in it, the original stays", wait_for(lambda: tree_of(os.path.join(dd, "tree")) == before and tree_of(os.path.join(recv_d, "tree")) == before, 8), (before, tree_of(dd)))
    V.wait_screen("Copied 1 item", 8)
    V.keys("circle"); V.wait_screen("dest/", 5)

    files_cursor(V, recv_d, "tree")
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Copy tree")
    V.wait_screen("Copying tree", 5)
    V.keys("cross"); V.wait_screen("t1.bin", 5)
    V.keys("triangle")
    check("a folder cannot be copied into itself", V.wait_screen("cannot be copied into itself", 6) and tree_of(os.path.join(recv_d, "tree")) == before, V.screen())
    V.stop(); VITAS.remove(V)

    # ---- another card: the rename fails, so the data is copied and then the original is removed
    recv2 = os.path.join(ROOT, "move-copy", "received")
    os.makedirs(os.path.join(recv2, "dest")); os.makedirs(os.path.join(recv2, "tree", "sub", "deep")); os.makedirs(os.path.join(recv2, "tree", "emptydir"))
    open(os.path.join(recv2, "tree", "one.bin"), "wb").write(rnd(200_000, 206))
    open(os.path.join(recv2, "tree", "sub", "two.bin"), "wb").write(rnd(70_000, 207))
    open(os.path.join(recv2, "tree", "sub", "deep", "three.bin"), "wb").write(rnd(1, 208))
    open(os.path.join(recv2, "tree", "zero.bin"), "wb").write(b"")
    big = rnd(2_500_000, 209)
    open(os.path.join(recv2, "big.bin"), "wb").write(big)
    V = Vita("move-copy", 53626, 53627, config={"alias": "Copier", "screensaver": 0}, env={"LS_FAIL_RENAME": "1", "LS_SLOW_MOVE_MS": "60"})
    VITAS.append(V)
    V.keys("r", "r", "r"); V.wait_screen("tab=Files screen=files", 6)
    d2 = os.path.join(recv2, "dest")
    before = tree_of(os.path.join(recv2, "tree"))
    files_cursor(V, recv2, "tree")
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Move tree")
    V.wait_screen("Moving tree", 5)
    open_folder(recv2, "dest"); V.wait_screen("Move here", 5)
    V.keys("triangle")
    check("between cards a progress dialog shows the copy", V.wait_screen("screen=move", 6) and V.wait_screen("Copied to the other card first", 3) and " %" in V.screen(), V.screen())
    check("the folder arrives complete (sub-folders, an empty folder, an empty file)", wait_for(lambda: tree_of(os.path.join(d2, "tree")) == before and not os.path.exists(os.path.join(recv2, "tree")), 20), (before, tree_of(d2)))
    check("the dialog closes and reports", V.wait_screen("Moved 1 item", 8) and "screen=files" in V.screen(), V.screen())
    V.keys("circle"); V.wait_screen("dest/", 5)

    # ---- cancel in the middle of a big copy: the original stays, the partial copy goes away
    files_cursor(V, recv2, "big.bin")
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Move big.bin")
    V.wait_screen("Moving big.bin", 5)
    open_folder(recv2, "dest"); V.wait_screen("Move here", 5)
    V.keys("triangle")
    V.wait_screen("screen=move", 6)
    time.sleep(0.6)
    V.keys("circle")
    check("cancelling reports it", V.wait_screen("Move cancelled", 8) and "screen=files" in V.screen(), V.screen())
    check("the original is untouched", os.path.exists(os.path.join(recv2, "big.bin")) and open(os.path.join(recv2, "big.bin"), "rb").read() == big)
    check("the half-written copy is removed", not os.path.exists(os.path.join(d2, "big.bin")), os.listdir(d2))

    # ---- and again, to the end
    V.keys("circle"); V.wait_screen("dest/", 5)
    files_cursor(V, recv2, "big.bin")
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Move big.bin")
    V.wait_screen("Moving big.bin", 5)
    open_folder(recv2, "dest"); V.wait_screen("Move here", 5)
    V.keys("triangle")
    check("a big file is copied completely and the original removed", wait_for(lambda: os.path.exists(os.path.join(d2, "big.bin")) and not os.path.exists(os.path.join(recv2, "big.bin")), 30) and open(os.path.join(d2, "big.bin"), "rb").read() == big, os.listdir(d2))
    check("the log has the summary", "move: 1 done, 0 failed" in V.log(), V.log()[-400:])
    V.wait_screen("Moved 1 item", 8)

    # ---- a big COPY can be cancelled too; the original is never touched
    big2 = rnd(2_500_000, 214)
    open(os.path.join(recv2, "big2.bin"), "wb").write(big2)
    V.keys("circle"); V.wait_screen("dest/", 5)
    V.keys("r"); time.sleep(0.4); V.keys("l"); V.wait_screen("tab=Files screen=files", 5)          # (the folder is listed again when the tab is opened)
    files_cursor(V, recv2, "big2.bin")
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Copy big2.bin")
    V.wait_screen("Copying big2.bin", 5)
    open_folder(recv2, "dest"); V.wait_screen("Copy here", 5)
    V.keys("triangle")
    check("the copy dialog says the originals stay", V.wait_screen("screen=move", 6) and V.wait_screen("The originals stay where they are", 3), V.screen())
    time.sleep(0.6)
    V.keys("circle")
    check("cancelling a copy reports it", V.wait_screen("Copy cancelled", 8) and "screen=files" in V.screen(), V.screen())
    check("... the original is there and the half-written copy is gone", open(os.path.join(recv2, "big2.bin"), "rb").read() == big2 and not os.path.exists(os.path.join(d2, "big2.bin")), os.listdir(d2))
    V.keys("circle"); V.wait_screen("dest/", 5)
    files_cursor(V, recv2, "big2.bin")
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Copy big2.bin")
    V.wait_screen("Copying big2.bin", 5)
    open_folder(recv2, "dest"); V.wait_screen("Copy here", 5)
    V.keys("triangle")
    check("a big copy completes and keeps the original", wait_for(lambda: os.path.exists(os.path.join(d2, "big2.bin")) and open(os.path.join(d2, "big2.bin"), "rb").read() == big2 and os.path.exists(os.path.join(recv2, "big2.bin")), 30), os.listdir(d2))
    V.wait_screen("Copied 1 item", 8)
    check("the log tells copy and move apart", wait_for(lambda: "copy: 1 done, 0 failed" in V.log(), 5), V.log()[-500:])
    V.stop(); VITAS.remove(V)

    # ---- not enough room on the other card
    recv3 = os.path.join(ROOT, "move-full", "received")
    os.makedirs(os.path.join(recv3, "dest"))
    open(os.path.join(recv3, "huge.bin"), "wb").write(rnd(500_000, 210))
    V = Vita("move-full", 53628, 53629, config={"alias": "Full", "screensaver": 0}, env={"LS_FAIL_RENAME": "1"}, extra=["--free-space", "100000"])
    VITAS.append(V)
    V.keys("r", "r", "r"); V.wait_screen("tab=Files screen=files", 6)
    files_cursor(V, recv3, "huge.bin")
    V.keys("select"); V.wait_screen("Options", 5); V.menu_pick("Move huge.bin")
    V.wait_screen("Moving huge.bin", 5)
    open_folder(recv3, "dest"); V.wait_screen("Move here", 5)
    V.keys("triangle")
    check("not enough free space is reported before anything is copied", V.wait_screen("Not enough free space", 6) and os.path.exists(os.path.join(recv3, "huge.bin")) and not os.listdir(os.path.join(recv3, "dest")), V.screen())
    V.stop(); VITAS.remove(V)
    check("no sanitizer errors during the move tests", not sanitizer_clean([v for v in ALL_VITAS if v.name.startswith("move")]), sanitizer_clean([v for v in ALL_VITAS if v.name.startswith("move")]))


def test_wifi():
    print("== WI-FI: switched off at the start (question on the boot screen) and lost while the app runs (shown on the page itself)")
    BTN = dict(START=1, SELECT=2, TRI=4, O=8, X=16, SQ=32)
    def wf(name):
        p = os.path.join(ROOT, name + ".wifi"); open(p, "w").write("off\n"); return p
    def gone(V, secs=6): return wait_for(lambda: V.proc.poll() is not None, secs)

    # ---- 1. off at the start: ask, switch on, carry on
    f = wf("w1")
    V = Vita("w1", 53640, 53641, config={"alias": "WifiOne", "screensaver": 0}, env={"LS_WIFI_FILE": f, "LS_WIFI_DELAY_MS": "1500"}, ready=False)
    VITAS.append(V)
    check("Wi-Fi off at the start: the question appears", V.wait_screen("Wi-Fi is turned off", 8), V.screen())
    scr = V.screen()
    check("it says what Wi-Fi is needed for - in one short line", "VitaSend needs Wi-Fi to find other devices." in scr and "Settings > Network" not in scr and "Waiting for Wi-Fi" not in scr, scr)
    check("it offers 'Turn on Wi-Fi' (X) and 'Quit' (O), also on the hint bar", "Turn on Wi-Fi" in scr and "Quit" in scr and V.find_hit("K", BTN["X"]) and V.find_hit("K", BTN["O"]), V.hits())
    time.sleep(0.5)
    check("nothing happens by itself", V.has("Wi-Fi is turned off") and "switch-on requested" not in V.log(), V.log()[-300:])
    check("the app does not start without Wi-Fi", "This device" not in V.screen())
    V.tap_hit("K", BTN["X"], False)
    check("a tap on the X button starts switching it on", V.wait_screen("Turning on Wi-Fi...", 5) and V.has("Connecting to your network"), V.screen())
    check("while connecting: only Cancel is offered", V.has("Cancel") and not V.has("Turn on Wi-Fi") and V.find_hit("K", BTN["O"]) is not None, V.screen())
    check("the app starts by itself once the network is there", V.wait_screen("This device", 10) and V.has("Ready"), V.screen())
    log = V.log()
    check("the log tells what happened", "Wi-Fi: switch-on requested" in log and "Wi-Fi: connected" in log and "Wi-Fi connected, IP" in log, log[-500:])
    V.stop(); VITAS.remove(V)

    # ---- 2. cancel while connecting, then quit with O
    f = wf("w2")
    V = Vita("w2", 53642, 53643, config={"alias": "WifiTwo", "screensaver": 0}, env={"LS_WIFI_FILE": f, "LS_WIFI_TURNON": "nojoin", "LS_WIFI_TIMEOUT_MS": "60000"}, ready=False)
    VITAS.append(V)
    V.wait_screen("Wi-Fi is turned off", 8)
    V.keys("cross")
    check("X switches it on", V.wait_screen("Turning on Wi-Fi...", 5), V.screen())
    V.keys("circle")
    check("O while connecting cancels and goes back to the question", V.wait_screen("Wi-Fi is turned off", 5) and "waiting cancelled" in V.log(), V.screen())
    check("... and the app is still waiting (not quit)", V.proc.poll() is None)
    V.keys("circle")
    check("O on the question quits the app", gone(V, 6), V.proc.poll())
    VITAS.remove(V)

    # ---- 3. the system refuses: fall back to the Vita settings; the app keeps waiting and continues by itself
    f = wf("w3")
    V = Vita("w3", 53644, 53645, config={"alias": "WifiThree", "screensaver": 0, "wifiSettingsUri": "settings_dlg:test::page"}, env={"LS_WIFI_FILE": f, "LS_WIFI_TURNON": "fail"}, ready=False)
    VITAS.append(V)
    V.wait_screen("Wi-Fi is turned off", 8)
    V.keys("cross")
    check("refused: 'could not be turned on' with the way out", V.wait_screen("Wi-Fi could not be turned on", 5) and V.has("Settings > Network") and V.has("continues by itself") and V.has("Try again"), V.screen())
    check("the refusal and its reason are in the log", "could not be switched on from the app (not supported (test))" in V.log(), V.log()[-300:])
    V.keys("cross")
    time.sleep(0.4)
    check("'Try again' asks again", V.log().count("could not be switched on from the app") >= 2, V.log()[-400:])
    check("the 'could not be turned on' window offers TRIANGLE: Wi-Fi settings", V.has("Wi-Fi settings") and V.find_hit("K", BTN["TRI"], False) is not None, V.screen())
    V.keys("triangle")
    check("TRIANGLE opens the Wi-Fi settings, at the address from config.json (wifiSettingsUri)", wait_for(lambda: "opening the Wi-Fi settings" in V.log() and "address settings_dlg:test::page" in V.log(), 4), V.log()[-300:])
    open(f, "w").write("on\n")                              # the user turns it on in the Vita settings
    check("the app continues by itself when Wi-Fi comes", V.wait_screen("This device", 8), V.screen())
    V.stop(); VITAS.remove(V)

    # ---- 3b. the Wi-Fi settings cannot be opened: the window says so and stays
    f = wf("w8")
    V = Vita("w8", 53654, 53655, config={"alias": "WifiEight", "screensaver": 0}, env={"LS_WIFI_FILE": f, "LS_WIFI_TURNON": "fail", "LS_WIFI_SETTINGS_FAIL": "1"}, ready=False)
    VITAS.append(V)
    V.wait_screen("Wi-Fi is turned off", 8)
    V.keys("cross")
    V.wait_screen("Wi-Fi could not be turned on", 5)
    V.keys("triangle")
    check("settings cannot be opened: a note in the window, the app stays", V.wait_screen("could not be opened from here", 4) and V.has("Wi-Fi could not be turned on"), V.screen())
    check("... and the log says so", "could not open the Wi-Fi settings" in V.log(), V.log()[-300:])
    V.keys("circle")
    check("O still quits", gone(V, 6))
    VITAS.remove(V)

    # ---- 4. accepted, but no network ever comes: after the waiting time, the same fallback
    f = wf("w4")
    V = Vita("w4", 53646, 53647, config={"alias": "WifiFour", "screensaver": 0}, env={"LS_WIFI_FILE": f, "LS_WIFI_TURNON": "nojoin", "LS_WIFI_TIMEOUT_MS": "1500"}, ready=False)
    VITAS.append(V)
    V.wait_screen("Wi-Fi is turned off", 8)
    V.keys("cross")
    check("accepted but no network in time: the fallback appears", V.wait_screen("Turning on Wi-Fi...", 3) and V.wait_screen("Wi-Fi could not be turned on", 8), V.screen())
    check("... and the log says why", "no network address after the switch-on request" in V.log(), V.log()[-300:])
    V.keys("start")
    check("START quits from any state", gone(V, 6))
    VITAS.remove(V)

    # ---- 4b. "Continue without Wi-Fi": the app starts offline (Files tab etc.); the server starts when Wi-Fi comes
    f = wf("w7")
    V = Vita("w7", 53652, 53653, config={"alias": "WifiSeven", "screensaver": 0}, env={"LS_WIFI_FILE": f, "LS_WIFI_TURNON": "fail"}, ready=False)
    VITAS.append(V)
    V.wait_screen("Wi-Fi is turned off", 8)
    check("the question offers 'Continue without Wi-Fi' (SQUARE)", V.has("Continue without Wi-Fi") and V.find_hit("K", BTN["SQ"], False) is not None, V.screen())
    V.keys("cross")
    V.wait_screen("Wi-Fi could not be turned on", 5)
    check("... and so does the 'could not be turned on' dialog", V.has("Continue without Wi-Fi") and V.has("Try again") and V.find_hit("K", BTN["SQ"], False) is not None, V.screen())
    V.keys("square")
    check("Square starts the app without Wi-Fi", V.wait_screen("This device", 6) and V.has("No Wi-Fi") and V.has("Wi-Fi not connected") and V.has("Turn on Wi-Fi"), V.screen())
    check("... without a server and without a leftover failure message", "continuing without Wi-Fi" in V.log() and "HTTPS server listening" not in V.log() and "use Settings" not in V.screen(), V.log()[-300:])
    V.keys("r", "r", "r")
    check("the other tabs work offline (Files)", V.wait_screen("tab=Files screen=files", 5), V.screen())
    V.keys("l", "l", "l")
    V.wait_screen("tab=Receive", 5)
    open(f, "w").write("on\n")                              # Wi-Fi comes (the user switched it on in the Vita settings)
    check("when Wi-Fi comes, the server starts and the card is 'Ready'", V.wait_screen("Ready", 8) and wait_for(lambda: "HTTPS server listening" in V.log(), 5), V.log()[-400:])
    st, body = raw_request(V.port, "GET", "/api/localsend/v2/info")
    check("... and other devices can reach it", st == 200 and b"WifiSeven" in body, (st, body[:100]))
    V.stop(); VITAS.remove(V)

    # ---- 5. Wi-Fi lost while the app runs: shown on the Receive page, no popup; switched on again from there
    f = os.path.join(ROOT, "w5.wifi")
    V = Vita("w5", 53648, 53649, config={"alias": "WifiFive", "screensaver": 0}, env={"LS_WIFI_FILE": f, "LS_WIFI_DELAY_MS": "1500"})
    VITAS.append(V)
    check("Wi-Fi on: nothing about Wi-Fi is shown", V.has("Ready") and not V.has("No Wi-Fi") and not V.has("Turn on Wi-Fi"), V.screen())
    open(f, "w").write("off\n")
    check("Wi-Fi drops: the card says so ('No Wi-Fi', 'Wi-Fi not connected')", V.wait_screen("Wi-Fi not connected", 6) and V.has("No Wi-Fi") and not V.has("Ready"), V.screen())
    check("... with a 'Turn on Wi-Fi' button, and no popup", V.has("Turn on Wi-Fi") and "Wi-Fi is turned off" not in V.screen() and "screen=receive" in V.screen(), V.screen())
    check("... and the hint bar offers it too (also while the tab list has the focus)", V.screen().count("Turn on Wi-Fi") >= 2, V.screen())
    V.keys("triangle")
    check("Triangle starts it: 'Connecting' and 'Turning on Wi-Fi...'", V.wait_screen("Turning on Wi-Fi...", 4) and V.has("Connecting"), V.screen())
    check("... the button is gone while it connects", wait_for(lambda: V.has("Turning on Wi-Fi...") and not V.has("Turn on Wi-Fi") and not V.has("Wi-Fi not connected"), 3) or V.has("Ready"), V.screen())
    check("when the network is back the card is 'Ready' again", V.wait_screen("Ready", 10) and not V.has("Wi-Fi not connected") and not V.has("Turning on Wi-Fi"), V.screen())
    check("the log has the whole story", "Wi-Fi lost" in V.log() and "switch-on requested" in V.log() and "Wi-Fi address is now" in V.log(), V.log()[-500:])

    # on another page the sidebar row is a button too (touch)
    open(f, "w").write("off\n")
    V.wait_screen("Wi-Fi not connected", 6)
    V.keys("r")
    V.wait_screen("tab=Send", 4)
    side = [h for h in V.hits() if h[0] == "C" and int(h[1]) == 8 and int(h[2]) > 400]
    check("the sidebar row is tappable", len(side) == 1, V.hits())
    if side:
        V.tap(int(side[0][1]) + 40, int(side[0][2]) + 15)
        check("a tap on it switches Wi-Fi on", wait_for(lambda: V.log().count("switch-on requested") >= 2, 5) and V.has("tab=Send"), V.log()[-300:])
    check("... and it comes back", wait_for(lambda: V.log().count("Wi-Fi address is now") >= 2, 10), V.log()[-400:])
    V.keys("l"); V.wait_screen("tab=Receive", 4)
    check("the Receive page is 'Ready' again", V.wait_screen("Ready", 5))

    # refused in the app: say so on the card
    f2 = os.path.join(ROOT, "w6.wifi")
    V.stop(); VITAS.remove(V)
    V = Vita("w6", 53650, 53651, config={"alias": "WifiSix", "screensaver": 0}, env={"LS_WIFI_FILE": f2, "LS_WIFI_TURNON": "fail"})
    VITAS.append(V)
    open(f2, "w").write("off\n")
    V.wait_screen("Wi-Fi not connected", 6)
    V.keys("triangle")
    check("refused in the app: the card points to the Vita settings", V.wait_screen("use Settings > Network", 4) and V.has("No Wi-Fi"), V.screen())
    check("... its button now says 'Wi-Fi settings' and the hint bar offers TRIANGLE", V.has("Wi-Fi settings"), V.screen())
    V.keys("triangle")
    check("TRIANGLE on the card opens the Wi-Fi settings (built-in address when config.json has none)", wait_for(lambda: "opening the Wi-Fi settings" in V.log() and "address default" in V.log(), 4), V.log()[-300:])
    open(f2, "w").write("on\n")
    check("... and recovers by itself when Wi-Fi comes back", V.wait_screen("Ready", 8), V.screen())
    V.stop(); VITAS.remove(V)
    bad = sanitizer_clean([v for v in ALL_VITAS if re.match(r"w\d$", v.name)])
    check("no sanitizer errors during the Wi-Fi tests", not bad, bad)

def make_pictures(d):
    """Test pictures: a 4-colour JPEG, a PNG with a transparent half, a BMP, a JPEG with EXIF orientation 6, and too-big ones."""
    from PIL import Image
    os.makedirs(d, exist_ok=True)
    q = Image.new("RGB", (640, 480))
    for x in range(640):
        for y in range(480):
            q.putpixel((x, y), ((255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0))[(x >= 320) + 2 * (y >= 240)])
    q.save(os.path.join(d, "pic1.jpg"), quality=95, subsampling=0)
    a = Image.new("RGBA", (200, 100), (0, 0, 0, 0))
    for x in range(100):
        for y in range(100): a.putpixel((x, y), (255, 0, 255, 255))
    a.save(os.path.join(d, "pic2.png"))
    Image.new("RGB", (120, 80), (0, 200, 200)).save(os.path.join(d, "pic3.bmp"))
    r = Image.new("RGB", (300, 200), (255, 0, 0))            # stored: red left half, blue right half; orientation 6 = shown turned 90 degrees clockwise
    for x in range(150, 300):
        for y in range(200): r.putpixel((x, y), (0, 0, 255))
    ex = Image.Exif(); ex[0x0112] = 6
    r.save(os.path.join(d, "rot.jpg"), quality=95, subsampling=0, exif=ex)
    Image.new("RGB", (4000, 3000), (10, 20, 30)).save(os.path.join(d, "huge.jpg"))             # 12 million pixels: refused
    Image.new("RGB", (1800, 1500), (10, 20, 30)).save(os.path.join(d, "prog.jpg"), progressive=True)   # progressive, 2.7 million: refused
    open(os.path.join(d, "bad.jpg"), "wb").write(b"this is not a picture")

def test_view():
    print("== VIEW: opening pictures and text files from the Files tab; X opens, SQUARE ticks, queues, slim options menu")
    from PIL import Image
    BTN = dict(START=1, SELECT=2, TRI=4, O=8, X=16, SQ=32)
    recv = os.path.join(ROOT, "view", "received")
    make_pictures(recv)
    open(os.path.join(recv, "notes.txt"), "w").write("\n".join(f"line {i:03d} of the notes" for i in range(1, 121)) + "\n")
    open(os.path.join(recv, "long.md"), "w").write("# Title\r\n" + "word " * 400 + "\r\n\ttabbed line \u00e9\u00e8\r\n")
    open(os.path.join(recv, "blob.txt"), "wb").write(b"abc\x00def")
    open(os.path.join(recv, "data.bin"), "wb").write(rnd(300, 301))
    make_vpk(os.path.join(recv, "AppOne.vpk"), title="App One", tid="APPO00001", ver="01.00")
    make_vpk(os.path.join(recv, "AppTwo.vpk"), title="App Two", tid="APPT00002", ver="02.00")
    ppm = os.path.join(ROOT, "view", "screen.ppm")
    V = Vita("view", 53670, 53671, config={"alias": "Viewer", "screensaver": 0}, extra=["--dump-ppm", ppm])
    VITAS.append(V)
    V.keys("r", "r", "r")
    check("Files tab opens", V.wait_screen("tab=Files screen=files", 6) and "pic1.jpg" in V.screen(), V.screen())
    def px(x, y):
        time.sleep(0.5)
        return Image.open(ppm).convert("RGB").getpixel((x, y))
    def pxw(x, y, want):
        """the pixel, waiting (the ppm dump is written at most every 250 ms) until it is near the wanted colour"""
        c = px(x, y)
        for _ in range(12):
            if all(abs(a - b) < 30 for a, b in zip(c, want)): break
            c = px(x, y)
        return c
    def close_view():
        V.keys("circle"); return V.wait_screen("screen=files", 5)

    # ---- the hint bar of the Files tab shows what X does for the highlighted entry
    files_cursor(V, recv, "pic1.jpg")
    check("on a picture the hint says X View, SQUARE Tick, TRIANGLE Send", V.has("View") and V.has("Tick") and V.has("Send") and not V.has("Tabs"), V.screen())
    # ---- one picture
    V.keys("cross")
    check("X opens a picture in the viewer", V.wait_screen("screen=view", 5) and V.wait_screen("640 x 480", 6) and V.has("pic1.jpg"), V.screen())
    scr = V.screen()
    check("a single picture: only 'Close' on the hint bar", "Close" in scr and "Previous" not in scr, scr)
    # shown at 608 x 456, centred in 960 x 456 below the 44 px bar: the quadrants are where they should be
    ox, oy = (960 - 608) // 2, 44
    cols = (pxw(ox + 100, oy + 100, (255, 0, 0)), pxw(ox + 500, oy + 100, (0, 255, 0)), pxw(ox + 100, oy + 350, (0, 0, 255)), pxw(ox + 500, oy + 350, (255, 255, 0)))
    near = lambda c, t: all(abs(a - b) < 30 for a, b in zip(c, t))
    check("the picture is drawn with the right colours in the right places", near(cols[0], (255, 0, 0)) and near(cols[1], (0, 255, 0)) and near(cols[2], (0, 0, 255)) and near(cols[3], (255, 255, 0)), cols)
    check("the area around it is the background", sum(px(20, 300)) < 80, px(20, 300))
    check("O closes it and the Files tab is back", close_view())
    # ---- EXIF orientation
    files_cursor(V, recv, "rot.jpg"); V.keys("cross")
    check("a JPEG with EXIF orientation is shown upright (the size says 200 x 300)", V.wait_screen("200 x 300", 6), V.screen())
    top, bottom = pxw(480, 44 + 60, (255, 0, 0)), pxw(480, 44 + 400, (0, 0, 255))
    check("orientation 6: the stored left half (red) is now on top, the right half (blue) below", near(top, (255, 0, 0)) and near(bottom, (0, 0, 255)), (top, bottom))
    close_view()
    # ---- transparent PNG
    files_cursor(V, recv, "pic2.png"); V.keys("cross")
    V.wait_screen("200 x 100", 6)
    left, right = px(480 - 150, 44 + 228), px(480 + 150, 44 + 228)
    check("PNG: opaque part drawn, transparent part shows the background", near(left, (255, 0, 255)) and sum(right) < 80, (left, right))
    close_view()
    # ---- refused pictures
    for name, why in (("huge.jpg", "too large"), ("prog.jpg", "too large"), ("bad.jpg", "Cannot show")):
        files_cursor(V, recv, name); V.keys("cross")
        check(f"{name}: a clear message instead of a crash ({why})", V.wait_screen(why, 8), V.screen())
        close_view()
    # ---- text
    files_cursor(V, recv, "notes.txt")
    check("on a text file the hint says X View", V.has("View"))
    V.keys("cross")
    check("X opens a text file", V.wait_screen("screen=view", 5) and V.wait_screen("line 001 of the notes", 6), V.screen())
    check("the hint bar offers scrolling and Close", V.has("Scroll") and V.has("Close"), V.screen())
    V.keys("down", "down", "down")
    check("DOWN scrolls the text", wait_for(lambda: "line 001 of the notes" not in V.screen() and "line 004 of the notes" in V.screen(), 4), V.screen())
    V.keys("right")
    check("RIGHT scrolls a page", V.wait_screen("line 021 of the notes", 3), V.screen())
    V.keys("down", "down", "down", "down", "down", "down", "down", "down", "down", "down", "down", "down", "down", "down", "down", "down", "down", "down", "down", "down", "down", "down", "down", "down")
    close_view()
    files_cursor(V, recv, "long.md"); V.keys("cross")
    check("a long line is wrapped", V.wait_screen("# Title", 6) and V.screen().count("word word word") >= 10, V.screen())
    V.keys("right", "right")
    check("tabs and CR/LF are handled, accents are shown", V.wait_screen("tabbed line", 4) and "\u00e9\u00e8" in V.screen() and "\r" not in V.screen(), V.screen())
    close_view()
    files_cursor(V, recv, "blob.txt"); V.keys("cross")
    check("a file with binary data is refused as text", V.wait_screen("not a text file", 6), V.screen())
    close_view()
    # ---- a file that cannot be opened
    files_cursor(V, recv, "data.bin")
    check("on other files the hint has no X entry", not V.has("X  View") and not V.has("Install") and V.has("Tick"), V.screen())
    V.keys("cross")
    check("X on another file says so (and nothing opens)", V.wait_screen("cannot be opened here", 4) and V.has("screen=files"), V.screen())
    # ---- SQUARE ticks
    files_cursor(V, recv, "pic1.jpg")
    V.keys("square")
    check("SQUARE ticks (and X no longer ticks)", V.wait_screen("1 selected", 4), V.screen())
    V.keys("square")
    check("SQUARE again unticks", wait_for(lambda: "selected" not in V.screen(), 4), V.screen())
    # ---- several pictures: a queue
    for n in ("pic1.jpg", "pic2.png", "pic3.bmp"):
        files_cursor(V, recv, n); V.keys("square")
    check("three ticked", V.wait_screen("3 selected", 4), V.screen())
    files_cursor(V, recv, "pic1.jpg")
    check("with several ticked the hint says X View ticked", V.has("View ticked"), V.screen())
    V.keys("cross")
    check("X opens the ticked pictures as a queue", V.wait_screen("screen=view", 5) and V.wait_screen("640 x 480", 8) and V.has("1 / 3") and V.has("pic1.jpg"), V.screen())
    check("the hint bar offers Previous / next", V.has("Previous") and V.has("Close"), V.screen())
    V.keys("right")
    check("RIGHT shows the next picture", V.wait_screen("200 x 100", 8) and V.has("2 / 3") and V.has("pic2.png"), V.screen())
    V.keys("r")
    check("R shows the next one", V.wait_screen("120 x 80", 8) and V.has("3 / 3") and V.has("pic3.bmp"), V.screen())
    check("BMP is drawn", near(pxw(480, 44 + 228, (0, 200, 200)), (0, 200, 200)))
    V.keys("right")
    time.sleep(0.4)
    check("at the end it stays on the last picture", V.has("3 / 3"))
    V.keys("l", "left")
    check("L and LEFT go back", V.wait_screen("640 x 480", 8) and V.has("1 / 3") and V.has("pic1.jpg"), V.screen())
    V.tap(900, 250)
    check("tapping the right part of the screen is 'next'", V.wait_screen("2 / 3", 6), V.screen())
    close_view()
    check("closing keeps the ticks", V.has("3 selected"), V.screen())
    # ---- text and picture mixed in one queue
    files_cursor(V, recv, "pic1.jpg"); V.keys("square")
    files_cursor(V, recv, "pic2.png"); V.keys("square")
    files_cursor(V, recv, "pic3.bmp"); V.keys("square")           # (untick the three)
    files_cursor(V, recv, "notes.txt"); V.keys("square")
    files_cursor(V, recv, "pic3.bmp"); V.keys("square")
    V.wait_screen("2 selected", 4)
    files_cursor(V, recv, "notes.txt")
    V.keys("cross")
    check("pictures and texts can be in one queue", V.wait_screen("line 001 of the notes", 8) and V.has("1 / 2") and V.has("notes.txt"), V.screen())
    V.keys("r")                                                   # (LEFT/RIGHT page a text; R steps)
    check("... the next one is the picture", V.wait_screen("120 x 80", 8) and V.has("2 / 2") and V.has("pic3.bmp"), V.screen())
    close_view()
    files_cursor(V, recv, "pic3.bmp"); V.keys("square"); files_cursor(V, recv, "notes.txt"); V.keys("square")
    # ---- packages
    files_cursor(V, recv, "AppOne.vpk")
    check("on a package the hint says X Install", V.has("Install"), V.screen())
    V.keys("cross")
    check("X on one package asks 'Install this app?'", V.wait_screen("Install this app?", 5) and V.has("App One"), V.screen())
    V.keys("circle"); V.wait_screen("screen=files", 4)
    V.keys("square"); files_cursor(V, recv, "AppTwo.vpk"); V.keys("square")
    V.wait_screen("2 selected", 4)
    check("two ticked packages: the hint says X Install ticked", V.has("Install ticked"), V.screen())
    V.keys("cross")
    check("X asks 'Install these apps?' with the list", V.wait_screen("Install these apps?", 5) and V.has("App One") and V.has("App Two"), V.screen())
    V.keys("circle"); V.wait_screen("screen=files", 4)
    files_cursor(V, recv, "pic1.jpg"); V.keys("square")
    V.wait_screen("3 selected", 4)
    V.keys("cross")
    check("packages and pictures ticked together: a clear message", V.wait_screen("Tick only packages", 4) and V.has("screen=files"), V.screen())
    # ---- the options menu has no entries that the hint bar already offers
    V.keys("select"); V.wait_screen("Options", 5)
    items = V.menu_items()
    check("the options menu does not repeat Install / Tick / Send", not any(t.startswith(("Install", "Send", "Tick the", "Untick ")) and not t.startswith("Untick everything") for t in items) and not any(t.startswith("Install") or t.startswith("Send") for t in items), items)
    check("... but still has the rest (tick all, untick, copy, move, rename, delete, new folder)", any(t.startswith("Tick everything") for t in items) and any(t.startswith("Copy") for t in items) and any(t.startswith("Move") for t in items) and any(t.startswith("Delete") for t in items) and any(t.startswith("New folder") for t in items), items)
    V.keys("circle"); V.wait_screen("screen=files", 4)
    # ---- the "L R Tabs" hint is only on the Receive tab
    V.keys("l", "l", "l")                                             # Files -> History -> Send -> Receive
    V.wait_screen("tab=Receive", 5)
    check("Receive: the hint bar has 'Tabs'", V.wait_screen("Tabs", 3), V.screen())
    for key, tab in (("r", "Send"), ("r", "History"), ("r", "Files"), ("r", "Settings")):
        V.keys(key); V.wait_screen("tab=" + tab, 4); time.sleep(0.3)
        check(f"{tab}: no 'Tabs' on the hint bar (L / R still work)", "Tabs" not in V.screen(), V.screen())
    V.stop(); VITAS.remove(V)
    bad = sanitizer_clean([v for v in ALL_VITAS if v.name == "view"])
    check("no sanitizer errors in the viewer tests", not bad, bad)


def test_fileops():
    print("== FILE OPS: deleting with a progress bar (and stopping it), unzipping a zip file (progress, safety, cancel)")
    recv = os.path.join(ROOT, "fops", "received")
    os.makedirs(recv)
    def make_tree(name, dirs=5, files=10):
        for d in range(dirs):
            os.makedirs(os.path.join(recv, name, f"d{d}"))
            for f in range(files): open(os.path.join(recv, name, f"d{d}", f"f{f}.bin"), "wb").write(b"x" * 100)
    make_tree("tree1"); make_tree("tree2")
    # ---- zip files
    good = {"readme.txt": b"hello zip\n" * 30, "docs/a.txt": b"alpha " * 500, "docs/deep/b.bin": rnd(40000, 9), "pic.bin": rnd(3000, 10)}
    with zipfile.ZipFile(os.path.join(recv, "good.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        for n, b in good.items(): z.writestr(n, b)
        z.writestr(zipfile.ZipInfo("emptydir/"), b"")
    with zipfile.ZipFile(os.path.join(recv, "stored.zip"), "w", zipfile.ZIP_STORED) as z: z.writestr("s.txt", b"stored!")
    with zipfile.ZipFile(os.path.join(recv, "evil.zip"), "w") as z:
        z.writestr("ok.txt", b"fine"); z.writestr("../escape.txt", b"owned")
    with zipfile.ZipFile(os.path.join(recv, "evil2.zip"), "w") as z: z.writestr("/abs.txt", b"owned")
    with zipfile.ZipFile(os.path.join(recv, "empty.zip"), "w") as z: pass
    open(os.path.join(recv, "notzip.zip"), "wb").write(rnd(4000, 11))
    with zipfile.ZipFile(os.path.join(recv, "crc.zip"), "w", zipfile.ZIP_STORED) as z:
        z.writestr("first.txt", b"first file ok"); z.writestr("second.txt", b"CORRUPT-ME-HERE" * 20)
    raw = bytearray(open(os.path.join(recv, "crc.zip"), "rb").read()); i = raw.find(b"CORRUPT-ME"); raw[i + 2] ^= 0x55
    open(os.path.join(recv, "crc.zip"), "wb").write(raw)
    with zipfile.ZipFile(os.path.join(recv, "big.zip"), "w", zipfile.ZIP_STORED) as z:
        for i in range(12): z.writestr(f"part{i:02d}.bin", rnd(200000, 30 + i))
    os.makedirs(os.path.join(recv, "good"))                                   # the name "good" is taken: the result gets "good (1)"
    open(os.path.join(recv, "good", "mine.txt"), "w").write("keep me")
    V = Vita("fops", 53830, 53831, config={"alias": "FileOps", "screensaver": 0}, env={"LS_SLOW_DELETE_MS": "40", "LS_SLOW_UNZIP_MS": "60"})
    VITAS.append(V)
    V.keys("r", "r", "r")
    check("Files tab opens", V.wait_screen("tab=Files screen=files", 6) and "tree1/" in V.screen(), V.screen())
    check("zip files are marked", V.screen().count("ZIP") >= 5, V.screen())

    # ---- delete with progress
    files_cursor(V, recv, "tree1")
    V.keys("select"); V.wait_screen("Options", 5)
    V.menu_pick("Delete tree1")
    V.wait_screen("Delete?", 5); V.keys("cross")
    check("deleting shows a progress dialog", V.wait_screen("screen=delete", 5) and V.wait_screen("Deleting", 2), V.screen())
    check("... that counts files and folders (1 + 5 folders + 50 files = 56)", wait_for(lambda: "of 56 files and folders" in V.screen(), 5), V.screen())
    check("... names what is going on and offers Stop", "tree1" in V.screen() and "Stop" in V.screen() and V.find_hit("K", 8) is not None, V.screen())
    seen = []
    def prog():
        for l in V.screen().splitlines():
            if " of 56 files" in l:
                try: seen.append(int(l.split(" of ")[0].strip().split()[-1]))
                except Exception: pass
        return len(set(seen)) >= 3
    check("the bar really moves", wait_for(prog, 5), seen)
    check("it finishes: gone, and a message", wait_for(lambda: not os.path.exists(os.path.join(recv, "tree1")), 15) and V.wait_screen("Deleted 1 item", 6) and "screen=files" in V.screen(), os.listdir(recv))

    # ---- stop half way
    files_cursor(V, recv, "tree2")
    V.keys("select"); V.wait_screen("Options", 5)
    V.menu_pick("Delete tree2")
    V.wait_screen("Delete?", 5); V.keys("cross")
    V.wait_screen("screen=delete", 5); time.sleep(0.8)
    V.keys("circle")
    check("[O] stops it with a message", V.wait_screen("Stopped", 6) and V.wait_screen("screen=files", 5), V.screen())
    left = sum(len(f) for _, _, f in os.walk(os.path.join(recv, "tree2"))) if os.path.exists(os.path.join(recv, "tree2")) else -1
    check("... what was deleted stays deleted, the rest stays", 0 < left < 50, left)

    # ---- unzip: ask, progress, result
    files_cursor(V, recv, "good.zip")
    check("the hint bar offers X Unzip", "[X]  Unzip" in V.screen(), V.screen())
    V.keys("cross")
    check("X on a zip asks first", V.wait_screen("screen=unzip", 5) and V.wait_screen("Unzip this file?", 3), V.screen())
    scr = V.screen()
    check("... with the number of files and the new folder's name (taken names get (1))", "4 files" in scr and "good (1)" in scr and "stays as it is" in scr, scr)
    V.keys("circle")
    check("[O] cancels: nothing is created", V.wait_screen("screen=files", 5) and not os.path.exists(os.path.join(recv, "good (1)")), os.listdir(recv))
    V.keys("cross")
    V.wait_screen("Unzip this file?", 5); V.keys("cross")
    check("[X] unpacks it and says so", V.wait_screen("Unzipped", 10) and V.wait_screen("good (1)", 2), V.screen())
    ok = all(open(os.path.join(recv, "good (1)", n), "rb").read() == b for n, b in good.items()) if os.path.isdir(os.path.join(recv, "good (1)")) else False
    check("every file is intact (also in sub folders)", ok, os.listdir(recv))
    check("an empty folder inside is kept", os.path.isdir(os.path.join(recv, "good (1)", "emptydir")))
    check("the existing folder with the same name is untouched", open(os.path.join(recv, "good", "mine.txt")).read() == "keep me")
    check("the zip itself is still there", os.path.exists(os.path.join(recv, "good.zip")))
    V.keys("cross")
    check("[X] 'Show the folder' lands on the new folder", V.wait_screen("screen=files", 5) and wait_for(lambda: any(l.startswith(">") and "good (1)" in l for l in V.screen().splitlines()), 5), V.screen())

    # ---- unzip from the options menu; stored files; plain name
    files_cursor(V, recv, "stored.zip")
    V.keys("select"); V.wait_screen("Options", 5)
    check("the options menu has 'Unzip' for a zip", any(t.startswith("Unzip stored.zip") for t in V.menu_items()), V.menu_items())
    V.menu_pick("Unzip stored.zip")
    check("... which asks the same question", V.wait_screen("Unzip this file?", 5) and "Into a new folder: stored" in V.screen(), V.screen())
    V.keys("cross")
    check("a stored zip works", V.wait_screen("Unzipped", 10) and open(os.path.join(recv, "stored", "s.txt"), "rb").read() == b"stored!", os.listdir(recv))
    V.keys("cross"); V.wait_screen("screen=files", 5)

    # ---- unsafe, empty, damaged
    for name, why, text in (("evil.zip", "unsafe path with ..", "Unsafe file name"), ("evil2.zip", "absolute path", "Unsafe file name"),
                            ("empty.zip", "empty zip", "empty"), ("notzip.zip", "not a zip at all", None)):
        files_cursor(V, recv, name); V.keys("cross")
        okv = V.wait_screen("Cannot unzip this file", 5) and (text is None or text in V.screen())
        check(f"{why}: refused before anything is written", okv, V.screen())
        V.keys("cross"); V.wait_screen("screen=files", 5)
    check("nothing escaped and no folder was left behind", not os.path.exists(os.path.join(ROOT, "fops", "escape.txt")) and not os.path.exists(os.path.join(recv, "evil")) and not os.path.exists(os.path.join(recv, "evil2")), os.listdir(recv))
    files_cursor(V, recv, "crc.zip"); V.keys("cross")
    V.wait_screen("Unzip this file?", 5); V.keys("cross")
    check("a damaged file inside: the error names it", V.wait_screen("second.txt", 10) and V.wait_screen("checksum", 2), V.screen())
    V.keys("cross"); V.wait_screen("screen=files", 5)
    check("... and the half-unpacked folder is removed again", not os.path.exists(os.path.join(recv, "crc")), os.listdir(recv))

    # ---- cancel while unpacking
    files_cursor(V, recv, "big.zip"); V.keys("cross")
    V.wait_screen("Unzip this file?", 5); V.keys("cross")
    check("progress is shown while unpacking", V.wait_screen("Unzipping", 5) and wait_for(lambda: "/ 12" in V.screen(), 5), V.screen())
    time.sleep(0.8)
    check("... with a percentage", any(l.strip().endswith("%") and l.strip()[0].isdigit() for l in V.screen().splitlines()), V.screen())
    V.keys("circle")
    check("[O] cancels", V.wait_screen("Cancelled", 8), V.screen())
    V.keys("cross"); V.wait_screen("screen=files", 5)
    check("... and the new folder is removed (all or nothing)", wait_for(lambda: not os.path.exists(os.path.join(recv, "big")), 5), os.listdir(recv))

    check("no sanitizer errors", not sanitizer_clean([V]), sanitizer_clean([V]))
    V.stop(); VITAS.remove(V)

def test_wifi_grace():
    print("== WI-FI GRACE: right after waking the Vita, Wi-Fi may need a few seconds - the 'Wi-Fi is turned off' card waits")
    # ---- Wi-Fi comes up during the grace time: the card never appears
    f = os.path.join(ROOT, "g1.wifi"); open(f, "w").write("off\n")
    V = Vita("g1", 53840, 53841, config={"alias": "GraceOne", "screensaver": 0}, env={"LS_WIFI_FILE": f, "LS_WIFI_GRACE_MS": "4000"}, ready=False)
    VITAS.append(V)
    check("at first it only says it is waiting", V.wait_screen("Waiting for Wi-Fi", 4), V.screen())
    time.sleep(1.0)
    check("... and the question is not shown yet", "Wi-Fi is turned off" not in V.screen(), V.screen())
    open(f, "w").write("on\n")
    check("Wi-Fi arrives: VitaSend starts normally", V.wait_screen("This device", 10), V.screen())
    check("... the card never appeared", "Wi-Fi is turned off" not in V.log() and "Wi-Fi is turned off" not in V.screen())
    V.stop(); VITAS.remove(V)
    # ---- it does not come: after the grace time the question appears
    f = os.path.join(ROOT, "g2.wifi"); open(f, "w").write("off\n")
    t0 = time.time()
    V = Vita("g2", 53842, 53843, config={"alias": "GraceTwo", "screensaver": 0}, env={"LS_WIFI_FILE": f, "LS_WIFI_GRACE_MS": "2500"}, ready=False)
    VITAS.append(V)
    time.sleep(1.2)
    check("within the grace time there is no question", "Wi-Fi is turned off" not in V.screen(), V.screen())
    check("... after it, there is", V.wait_screen("Wi-Fi is turned off", 8), V.screen())
    check("... not earlier than the grace time", time.time() - t0 >= 2.4, time.time() - t0)
    V.stop(); VITAS.remove(V)

# =========================================================================== update (a fake GitHub over TLS)
class FakeGitHub:
    """An HTTPS server with its own certificate authority: the release list, a redirect to the "CDN", chunked or plain bodies."""
    def __init__(self, base, port, host_name="localhost"):
        import http.server
        self.port = port
        self.hits = []
        self.releases = []
        self.vpk = b""
        self.sums = b""
        self.chunked = False
        self.sums_name = "VitaSend-v0.9.8.vpk"
        me = self
        class H(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"
            def log_message(self, *a): pass
            def send_body(self, body, ctype="application/octet-stream", chunked=False):
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Connection", "close")
                if chunked:
                    self.send_header("Transfer-Encoding", "chunked"); self.end_headers()
                    for i in range(0, len(body), 7001):
                        c = body[i:i + 7001]
                        self.wfile.write(b"%x\r\n" % len(c) + c + b"\r\n")
                    self.wfile.write(b"0\r\n\r\n")
                else:
                    self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
                self.close_connection = True
            def do_GET(self):
                me.hits.append(self.path)
                if self.path.startswith("/repos/Trashman1111/vitasend/releases"):
                    self.send_body(json.dumps(me.releases).encode(), "application/json")
                elif self.path == "/dl/vpk":
                    self.send_response(302); self.send_header("Location", f"https://localhost:{me.port}/cdn/file.vpk"); self.send_header("Content-Length", "0"); self.send_header("Connection", "close"); self.end_headers()
                    self.close_connection = True
                elif self.path == "/cdn/file.vpk":
                    self.send_body(me.vpk, chunked=me.chunked)
                elif self.path == "/dl/sums":
                    self.send_body(me.sums, "text/plain")
                else:
                    self.send_response(404); self.send_header("Content-Length", "0"); self.send_header("Connection", "close"); self.end_headers(); self.close_connection = True
        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), H)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(os.path.join(base, host_name + ".pem"), os.path.join(base, host_name + ".key"))
        self.srv.socket = ctx.wrap_socket(self.srv.socket, server_side=True)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()
    def release(self, tag, pre=False, draft=False, vpk=True, sums=True, digest=None, name=None, size=None):
        name = name or f"VitaSend-{tag}.vpk"
        base = f"https://localhost:{self.port}"
        assets = []
        if vpk:
            a = {"name": name, "browser_download_url": base + "/dl/vpk", "size": len(self.vpk) if size is None else size}
            if digest: a["digest"] = "sha256:" + digest
            assets.append(a)
        if sums: assets.append({"name": "SHA256SUMS.txt", "browser_download_url": base + "/dl/sums", "size": 100})
        return {"tag_name": tag, "name": tag, "draft": draft, "prerelease": pre, "assets": assets}
    def stop(self):
        self.srv.shutdown(); self.srv.server_close()

def make_pki(base):
    os.makedirs(base, exist_ok=True)
    def run(*a): subprocess.run(a, cwd=base, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "ca.key", "-out", "ca.pem", "-days", "30", "-subj", "/CN=VitaSend Test CA")
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", "evil.key", "-out", "evil.pem", "-days", "30", "-subj", "/CN=Other CA")
    for name in ("localhost", "other.test"):
        run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", name + ".key", "-out", name + ".csr", "-subj", "/CN=" + name)
        open(os.path.join(base, name + ".ext"), "w").write(f"subjectAltName=DNS:{name}\nbasicConstraints=CA:FALSE\n")
        run("openssl", "x509", "-req", "-in", name + ".csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-out", name + ".crt", "-days", "30", "-extfile", name + ".ext")
        open(os.path.join(base, name + ".pem"), "w").write(open(os.path.join(base, name + ".crt")).read() + open(os.path.join(base, "ca.pem")).read())

def test_update():
    print("== UPDATE: check at start (once a day, switchable), manual check, pre-releases, verified TLS, checksum, install")
    base = os.path.join(ROOT, "pki")
    make_pki(base)
    CA = os.path.join(base, "ca.pem")
    gh = FakeGitHub(base, 53790)
    new_vpk = os.path.join(ROOT, "new.vpk")
    make_vpk(new_vpk, title="VitaSend", tid="LSND00001", ver="00.98")
    gh.vpk = open(new_vpk, "rb").read()
    good = sha(gh.vpk)
    gh.sums = f"{good}  VitaSend-v0.9.8.vpk\n".encode()
    port = [53800]
    def vita(name, cfg=None, env=None, date="2026-11-06", delay="300", ca=CA, api=None):
        port[0] += 2
        c = {"alias": name, "screensaver": 0}; c.update(cfg or {})
        e = {"LS_UPDATE_API": api or f"localhost:{gh.port}", "LS_UPDATE_CA": ca, "LS_UPDATE_DELAY_MS": delay, "LS_FAKE_DATE": date}
        e.update(env or {})
        V = Vita(name, port[0], port[0] + 1, config=c, env=e)
        VITAS.append(V)
        return V
    def drop(V): V.stop(); VITAS.remove(V)
    today = 20261106

    # ---- 1. start-up: the newest release counts even when it is a pre-release; drafts do not
    gh.releases = [gh.release("v0.10.0", draft=True), gh.release("v0.9.9", pre=True), gh.release("v0.9.4")]
    gh.hits.clear()
    V = vita("upd1")
    check("the check at start finds the newest release by itself", V.wait_screen("screen=update", 12) and V.wait_screen("A new version is available", 5), V.screen() + V.log()[-600:])
    scr = V.screen()
    check("a pre-release is offered (and labelled), a draft is not", "v0.9.9" in scr and "pre-release" in scr and "v0.10.0" not in scr, scr)
    check("it shows what is installed and the size", "you have " + VERSION in scr and "MB" in scr, scr)
    check("the dialog offers Download and install / Not now", "Download and install" in scr and "Not now" in scr, scr)
    check("the question is remembered for today", wait_for(lambda: V.config().get("updateAskedDay") == today, 5), V.config())
    V.keys("circle")
    check("[O] 'Not now' goes back to where we were", wait_for(lambda: "screen=update" not in V.screen(), 5), V.screen())
    time.sleep(1.0)
    check("... and the same run does not ask again", "screen=update" not in V.screen(), V.screen())
    drop(V)

    # ---- 2. restarted on the same day: found again, but no question
    gh.hits.clear()
    V = vita("upd2", cfg={"updateAskedDay": today})
    check("the same day: the check runs", wait_for(lambda: any("/releases" in h for h in gh.hits), 10), gh.hits)
    check("... and says it quietly in the log", wait_for(lambda: "already asked today" in V.log(), 8), V.log()[-500:])
    time.sleep(0.8)
    check("... without a dialog", "screen=update" not in V.screen(), V.screen())
    # a manual check from the About box does show it, whatever was asked today
    V.goto_setting("about"); V.keys("cross")
    check("the manual check (About) still shows it", V.wait_screen("A new version is available", 12), V.screen())
    V.keys("circle")
    check("[O] on it returns to Settings", V.wait_screen("screen=settings", 5), V.screen())
    drop(V)

    # ---- 3. the next day: asked again; the highest number wins (0.10.0 is above 0.9.9)
    gh.releases = [gh.release("v0.9.9", pre=True), gh.release("v0.10.0", pre=True), gh.release("v0.9.4")]
    V = vita("upd3", cfg={"updateAskedDay": today}, date="2026-11-07")
    check("the next day: asked again, and 0.10.0 is above 0.9.9", V.wait_screen("A new version is available", 12) and "v0.10.0" in V.screen(), V.screen())
    check("... today's date is stored", wait_for(lambda: V.config().get("updateAskedDay") == 20261107, 5), V.config())
    drop(V)

    # ---- 4. switched off in Settings: nothing is asked of GitHub
    gh.hits.clear()
    V = vita("upd4", cfg={"checkUpdates": False})
    time.sleep(2.5)
    check("with the check switched off nothing is requested", not gh.hits and "screen=update" not in V.screen(), gh.hits)
    check("... and the log says so", "check at start is switched off" in V.log(), V.log()[-400:])
    V.goto_setting("checkupd")
    wait_for(lambda: "Check for updates at start" in V.screen() and "Off - only when you check" in V.screen(), 6)
    scr = V.screen()
    check("the Settings row says Off and what that means", "Check for updates at start" in scr and "Off" in scr and "About" in scr, scr)
    V.keys("cross")
    check("[X] switches it on and saves it", wait_for(lambda: V.config().get("checkUpdates") is True, 6) and V.wait_screen("On - looks for a new version", 5), V.config())
    V.keys("cross")
    check("[X] again switches it off", wait_for(lambda: V.config().get("checkUpdates") is False, 6), V.config())
    drop(V)

    # ---- 5. up to date: silent at start, a message from the About box
    gh.releases = [gh.release("v0.9.8"), gh.release("v0.9.4")]
    gh.hits.clear()
    V = vita("upd5")
    VER_NOW = VERSION
    gh.releases = [gh.release("v" + VER_NOW), gh.release("v0.9.4")]
    check("up to date: the start-up check asks GitHub", wait_for(lambda: any("/releases" in h for h in gh.hits), 10), gh.hits)
    time.sleep(1.0)
    check("... and stays silent", "screen=update" not in V.screen() and "This device" in V.screen(), V.screen())
    V.goto_setting("about"); V.keys("cross")
    check("the manual check says 'up to date'", V.wait_screen("VitaSend is up to date", 12), V.screen())
    check("... with both versions", "You have version " + VER_NOW in V.screen(), V.screen())
    V.keys("cross")
    check("[X] closes it", V.wait_screen("screen=settings", 5), V.screen())
    drop(V)

    # ---- 6. download (redirect + chunked body), checksum from SHA256SUMS.txt, then the self-update through the updater app
    #         (the running app cannot be replaced by the system installer: error 0x80101114 on a real Vita)
    appdir = os.path.join(ROOT, "appdir")
    old_vpk = os.path.join(ROOT, "old.vpk")
    make_vpk(old_vpk, title="VitaSend", tid="LSND00001", ver="00.97")
    zipfile.ZipFile(old_vpk).extractall(appdir)
    open(os.path.join(appdir, "eboot.bin"), "wb").write(make_eboot(False, seed=9))   # the installed (old) program differs from the new one
    old_eboot = open(os.path.join(appdir, "eboot.bin"), "rb").read()
    gh.releases = [gh.release("v0.9.8"), gh.release("v0.9.4")]
    gh.chunked = True
    V = vita("upd6", env={"LS_APP_DIR": appdir})
    check("v0.9.8 is offered", V.wait_screen("A new version is available", 12) and "v0.9.8" in V.screen() and "pre-release" not in V.screen(), V.screen())
    V.keys("cross")
    check("[X] downloads it (through the redirect)", V.wait_screen("Install the new VitaSend?", 20), V.screen() + V.log()[-800:])
    check("the downloaded file is kept and checked", wait_for(lambda: os.path.exists(os.path.join(V.data, "update", "VitaSend-v0.9.8.vpk")), 3) and sha(open(os.path.join(V.data, "update", "VitaSend-v0.9.8.vpk"), "rb").read()) == good)
    check("the installer says what happens to settings and files", V.has("Your settings and received files stay") and V.has("starts again by itself"), V.screen())
    V.keys("cross")
    check("the update is prepared and the updater is started", V.wait_screen("Finishing the update", 20), V.screen() + V.log()[-600:])
    check("... VitaSend ends itself (the updater app takes over)", wait_for(lambda: V.proc.poll() is not None, 10), V.log()[-500:])
    inst = os.path.join(V.data, "installed")
    check("the updater app is installed: a copy of the running eboot.bin under another title id",
          os.path.isdir(os.path.join(inst, "LSNDUPD01")) and open(os.path.join(inst, "LSNDUPD01", "eboot.bin"), "rb").read() == old_eboot, os.listdir(inst) if os.path.isdir(inst) else "no installed dir")
    hs = open(os.path.join(inst, "LSNDUPD01", "sce_sys", "param.sfo"), "rb").read() if os.path.isdir(os.path.join(inst, "LSNDUPD01")) else b""
    check("... with its own title id in param.sfo (and no trace of VitaSend's)", b"LSNDUPD01" in hs and b"LSND00001" not in hs)
    check("the updater installed the new version", os.path.isdir(os.path.join(inst, "LSND00001")) and open(os.path.join(inst, "LSND00001", "eboot.bin"), "rb").read() != old_eboot, os.listdir(inst))
    check("the log tells the story", all(t in V.log() for t in ("starting the updater app", "update helper: installing", "update helper: done")), V.log()[-600:])
    check("no leftovers in the package folder or next to it", not os.path.exists(os.path.join(V.data, "updater.vpk")) and not os.path.exists(os.path.join(V.data, "pkg")), os.listdir(V.data))
    check("the result is left for the next start", open(os.path.join(V.data, "update-result.txt")).read().strip() == "ok")
    # the next start of VitaSend reports it and removes the updater app
    subprocess.run([args.binary, "--data", V.data, "--port", "53897", "--seconds", "2"], env=dict(os.environ, LS_UPDATE_API="localhost:1"), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
    lg = open(os.path.join(V.data, "log.txt")).read()
    check("the next start says the update worked", "the updater app installed the new version" in lg, lg[-500:])
    check("... and removes the updater app again", "the updater app was removed again" in lg and not os.path.exists(os.path.join(inst, "LSNDUPD01")), os.listdir(inst))
    check("... and the result file is gone", not os.path.exists(os.path.join(V.data, "update-result.txt")))
    VITAS.remove(V)
    gh.chunked = False

    # the updater app cannot be made (this program's files cannot be read): a clear message, nothing is closed
    V = vita("upd6b")
    V.wait_screen("A new version is available", 12); V.keys("cross")
    V.wait_screen("Install the new VitaSend?", 20); V.keys("cross")
    check("no files of the installed VitaSend to copy: a clear message", V.wait_screen("Installation failed", 10) and V.wait_screen("Cannot read the files of the installed VitaSend", 2), V.screen())
    check("... and VitaSend keeps running", V.proc.poll() is None)
    drop(V)
    # the system refuses the updater app
    V = vita("upd6c", env={"LS_APP_DIR": appdir, "LS_FAKE_PROMOTE_FAIL": "1"})
    V.wait_screen("A new version is available", 12); V.keys("cross")
    V.wait_screen("Install the new VitaSend?", 20); V.keys("cross")
    check("the system refuses the updater: the reason is shown, VitaSend stays", V.wait_screen("Installation failed", 15) and V.wait_screen("simulated", 2) and V.proc.poll() is None, V.screen())
    check("... and the log has the result", wait_for(lambda: "Install: FAILED" in V.log(), 3), V.log()[-400:])
    drop(V)

    # ---- 7. a wrong checksum is refused and the file deleted
    gh.sums = f"{'0' * 64}  VitaSend-v0.9.8.vpk\n".encode()
    V = vita("upd7")
    V.wait_screen("A new version is available", 12); V.keys("cross")
    check("a wrong checksum: refused", V.wait_screen("Download failed", 20) and V.wait_screen("checksum does not match", 2), V.screen())
    check("... nothing stays on the card", not os.path.exists(os.path.join(V.data, "update", "VitaSend-v0.9.8.vpk")) and not os.path.exists(os.path.join(V.data, "update", "VitaSend-v0.9.8.vpk.part")))
    check("... and 'Try again' is offered", "Try again" in V.screen(), V.screen())
    gh.sums = f"{good}  VitaSend-v0.9.8.vpk\n".encode()
    V.keys("square")
    check("[SQUARE] tries the download again (now with the right checksum)", V.wait_screen("Install the new VitaSend?", 20), V.screen())
    drop(V)

    # ---- 8. no SHA256SUMS.txt: GitHub's own digest is used; none at all: refused
    gh.releases = [gh.release("v0.9.8", sums=False, digest=good)]
    V = vita("upd8")
    V.wait_screen("A new version is available", 12); V.keys("cross")
    check("without SHA256SUMS.txt the digest GitHub lists is used", V.wait_screen("Install the new VitaSend?", 20), V.screen() + V.log()[-500:])
    drop(V)
    gh.releases = [gh.release("v0.9.8", sums=False)]
    V = vita("upd9")
    V.wait_screen("A new version is available", 12); V.keys("cross")
    check("no checksum anywhere: the download is not trusted", V.wait_screen("Download failed", 15) and V.wait_screen("publishes no checksum", 2), V.screen())
    drop(V)

    # ---- 9. no .vpk on the release
    gh.releases = [gh.release("v0.9.8", vpk=False, sums=False)]
    V = vita("upd10")
    V.goto_setting("about"); V.keys("cross")
    check("a release without a package: a clear message", V.wait_screen("Update check failed", 12) and V.wait_screen("no .vpk", 2), V.screen())
    V.keys("circle")
    drop(V)

    # ---- 10. certificates are really checked
    gh.releases = [gh.release("v0.9.8")]
    V = vita("upd11", ca=os.path.join(base, "evil.pem"))
    V.goto_setting("about"); V.keys("cross")
    check("a certificate from an unknown authority is refused", V.wait_screen("Update check failed", 12) and "v0.9.8" not in V.screen(), V.screen())
    check("... the log says why", wait_for(lambda: "not trusted" in V.log().lower() or "verif" in V.log().lower(), 3), V.log()[-500:])
    drop(V)
    gh2 = FakeGitHub(base, 53792, host_name="other.test")
    gh2.releases = [gh2.release("v0.9.8")]
    V = vita("upd12", api="localhost:53792")
    V.goto_setting("about"); V.keys("cross")
    check("a certificate for another host name is refused", V.wait_screen("Update check failed", 12) and "A new version" not in V.screen(), V.screen())
    drop(V)
    gh2.stop()

    # ---- 11. GitHub is down / rate limited
    V = vita("upd13", api="localhost:53799")
    V.goto_setting("about"); V.keys("cross")
    check("nobody there: a message, not a hang", V.wait_screen("Update check failed", 20), V.screen())
    check("... 'Try again' (SQUARE) is offered", "Try again" in V.screen(), V.screen())
    drop(V)

    # ---- 12. the automatic question never interrupts a transfer request
    gh.releases = [gh.release("v0.9.9")]
    V = vita("upd14", delay="2500")
    files = [file_entry("id-1", "hello.bin", rnd(2000, 1))]
    t, box = prepare_async(V.port, files, alias="Banana PC")
    check("a request arrives first", V.wait_screen("Incoming request", 10), V.screen())
    time.sleep(4.5)
    check("the update question waits for it", V.has("Incoming request") and "screen=update" not in V.screen(), V.screen())
    V.keys("circle"); t.join(10)
    check("after the request is answered the question comes", V.wait_screen("A new version is available", 10), V.screen())
    drop(V)

    gh.stop()
    bad = sanitizer_clean([v for v in ALL_VITAS if v.name.startswith("upd")])
    check("no sanitizer errors in the update tests", not bad, bad)

# =========================================================================== run
try:
    if args.only == "shots":
        test_shots(); finish_all(); sys.exit(0)
    if args.only in ("", "recv"): test_receive()
    if args.only in ("", "extra"): test_extra()
    if args.only in ("", "send"): test_send()
    if args.only in ("", "install"): test_install()
    if args.only in ("", "privacy"): test_privacy()
    if args.only in ("", "theme"): test_theme()
    if args.only in ("", "touch"): test_touch()
    if args.only in ("", "migrate"): test_migrate()
    if args.only in ("", "files"): test_files()
    if args.only in ("", "exists"): test_exists()
    if args.only in ("", "retry"): test_retry()
    if args.only in ("", "queue"): test_queue()
    if args.only in ("", "polish"): test_polish()
    if args.only in ("", "dpad"): test_dpad()
    if args.only in ("", "x11"): test_x11()
    if args.only in ("", "x13"): test_x13()
    if args.only in ("", "move"): test_move()
    if args.only in ("", "wifi"): test_wifi(); test_wifi_grace()
    if args.only in ("", "view"): test_view()
    if args.only in ("", "update"): test_update()
    if args.only in ("", "fileops"): test_fileops()
finally:
    finish_all()

print()
passed = sum(results)
print(f"{passed}/{len(results)} checks passed")
if args.keep: print("kept:", ROOT)
else: shutil.rmtree(ROOT, ignore_errors=True)
sys.exit(0 if passed == len(results) else 1)
