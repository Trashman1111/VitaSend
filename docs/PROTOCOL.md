# LocalSend protocol notes

VitaSend speaks the **LocalSend protocol v2.1** ([specification](https://github.com/localsend/protocol)). This page lists
exactly what is implemented, how the code maps to the specification, and the few places where VitaSend deliberately behaves
in a particular way. It is an independent implementation - no code from the LocalSend apps is used.

## What is implemented

| Area | Support |
|---|---|
| Multicast discovery `224.0.0.167:53317` | yes - announce burst at start, then every 30 s, listener, `POST /register` back to whoever announces, UDP reply with `announce:false` if that fails |
| `GET /api/localsend/v2/info` | yes (`?fingerprint=` self-discovery check answers 412) |
| `POST /api/localsend/v2/register` | yes |
| `POST /api/localsend/v2/prepare-upload` | yes - receiving side (asks the user / Quick Save / PIN) and sending side (`?pin=`) |
| `POST /api/localsend/v2/upload` | yes - streamed, chunked or `Content-Length`, up to 4 in parallel |
| `POST /api/localsend/v2/cancel` | yes, both directions |
| `prepare-download` / `download` (browser download API) | **no** - answers 403 |
| Text messages | yes, both directions (a `text/plain` file entry with a `preview`) |
| HTTPS (TLS 1.2, self-signed P-256, fingerprint = SHA-256 of the certificate) | yes |
| HTTP (no TLS) | the server only speaks HTTPS; the client uses plain HTTP if a peer announces `protocol: "http"` |
| IPv6 | no |
| Peer certificate verification | no - same as the official apps. When sending, a certificate that differs from the announced fingerprint is written to the log (a note, not a refusal) |

Both the v2 field name `announce` and the v1 name `announcement` are sent and understood.

## Where it lives in the code

| Spec item | Code |
|---|---|
| announce / listen | `discovery.c` |
| `/info`, `/register`, routing | `api.c` |
| device info JSON | `identity.c` (`identity_json`) |
| receiving (`prepare-upload`, `upload`, `cancel`) | `rx.c` |
| sending | `send.c` |
| HTTP framing (chunked, 100-continue) | `http.c` |
| TLS, fingerprint | `tls.c` |

## Behaviour worth knowing

**Receiving**

* `prepare-upload` is answered only after the user decided (the HTTP thread waits), unless *Quick Save* accepts it. Answers:
  200 with a token per accepted file, 204 for an accepted text message, 401 wrong or missing PIN, 403 rejected, 409 busy
  (another transfer, or an installation, is running), 429 too many wrong PIN attempts (5 failures block everyone for a
  minute), 507 not enough space on the card (with 1 MB spare), 422 for a bad SHA-256 on upload.
* Files are written as `name.part` and renamed when complete; a cancelled or failed file is removed.
* The SHA-256 is verified when the sender provides one.
* File names are sanitised (`rx_sanitize_path`): `..` and empty parts are dropped, `\` and `/` both separate folders (at most
  8 levels deep), control characters and `<>:"|?*` become `_`, trailing dots and spaces are removed (FAT dislikes them), so
  everything ends up inside the receive folder.
* Up to 256 files per request.
* A name that already exists is handled by the setting *When a received file already exists*: keep both (`name (1).ext`),
  skip (if every file is skipped the sender is told there is nothing to send) or overwrite (the request dialog warns).

**Sending**

* One `POST /upload` per accepted file, each on a new TLS connection, streamed from the card.
* Handles a 200 that carries tokens for only some of the files (the others were declined), 204 for text, 401 (the Vita asks
  for the PIN), 403, 409, 429.
* After a failure *Retry* sends only the files that did not arrive.

**Discovery details**

* If the usual port is occupied VitaSend listens on the next free one and announces that port.
* The `deviceType` is `mobile`, the `deviceModel` is `PlayStation Vita`, the alias defaults to `Vita-XXXX` (from the
  fingerprint) and can be renamed in Settings.
* "Hide IP address and fingerprint" only hides things on this Vita's screen; nothing about the protocol changes.

## Deviations and limits

* One transfer at a time (the official apps allow parallel sessions).
* Every file uses its own TLS connection, so thousands of tiny files are slow on the Vita's CPU.
* Favourite / Quick Save trust the alias and fingerprint a sender *announces*. With self-signed certificates that cannot be
  verified, so on a network you do not trust keep Quick Save on *Ask*.
* Interoperability was checked against the LocalSend desktop app and against scripted stand-ins that follow the
  specification (`host/fake_peer.py`, `host/test_transfer.py`). If another LocalSend client misbehaves, a log
  (*Settings > Log*) and the client's name and version in an issue help a lot.
