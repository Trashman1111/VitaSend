# Contributing to VitaSend

Thank you for looking at the code! Bug reports, fixes, new features, documentation and testing on different Vita models
and firmwares are all welcome.

## Before you start

* Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - it explains the layers, the threads and where everything is.
* Build it: [docs/BUILDING.md](docs/BUILDING.md). Test it: [docs/TESTING.md](docs/TESTING.md).
* For anything bigger than a small fix, open an issue first so we can agree on the approach.

## The short version of a good change

1. **Build the PC test build and run the tests** - `make -C host SAN=1 && python3 host/fake_peer.py && python3 host/test_transfer.py`.
   A change should not add compiler warnings (`-Wall -Wextra` are on).
2. **Add or update a test** when you change behaviour (`host/test_transfer.py`; copy the pattern of a small existing test).
3. **Keep the platform boundary.** Only `plat_vita.c` / `ui_vita.c` may call Vita functions. New OS features go through
   `plat.h` with a stand-in in `plat_posix.c`.
4. **Say how you tested on hardware** if you touched `plat_vita.c`, `ui_vita.c` or anything the tests cannot reach
   (installer, Wi-Fi, touch scaling, power). It is fine to say "not tested on a Vita" - then a maintainer will.
5. **Update the docs**: [docs/USER_GUIDE.md](docs/USER_GUIDE.md) for visible behaviour, [CHANGELOG.md](CHANGELOG.md) for
   anything a user would notice.

## Code style

* Portable C (gnu11), 4 spaces, no tabs, lines up to about 130 characters, braces on the same line (see `.editorconfig`).
* A short comment at the top of every file saying what it is for; comments explain *why*, not what.
* Prefixes by module (`rx_`, `send_`, `ui_`, `plat_` ...). Static functions need no prefix.
* No new dependencies without discussion. The project deliberately has none besides mbedTLS and the VitaSDK stubs.
* Settings with more than two options are dropdown lists; two-option settings toggle with X. Every feature must work with
  buttons *and* touch (register hit areas while drawing, see `ui.h`).

## Settings, text and translations

The interface is English only for now. All strings are in the `app_*.c` files. If you want to add languages, open an issue
to discuss a clean way (a string table) before sending a big patch. Fonts cover Latin, Greek and Cyrillic; other scripts
need a bigger font and a different approach to memory.

## Ideas that would help

* Testing on other models (PCH-1000, PCH-2000, PlayStation TV) and firmwares; reports of what works.
* Parallel transfers, IPv6, the browser download API (`prepare-download`).
* A string table for translations.
* Faster rendering (the picture is drawn by the CPU) and faster TLS.
* A GitHub Actions job that also runs the Vita build (see `.github/workflows/`).

## License

By contributing you agree that your work is released under the project's license, **GPL-3.0-or-later** (see
[LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for why).
