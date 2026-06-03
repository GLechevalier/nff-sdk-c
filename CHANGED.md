# What changed

## Status — OTA verified working end-to-end (2026-06-04)

A full over-the-air update now succeeds on real hardware: `test-device-01` was updated
**1.0.0 → 1.0.1** over MQTT + HTTPS, rebooted into the new image, and the fleet heartbeat
reports `"fw":"1.0.1"`.

Getting there took **three layered fixes** — each was masking the next — plus reconciling
this repo with the deployed Arduino library so the fixes actually reach the device build:

1. **MQTT RX buffer** 256 → 1024 — the ~421 B OTA command was silently dropped.
2. **Signature buffer** `sig_hex[144]` → `[145]` — ~26% of *all* signed commands (incl. OTA)
   were rejected as `cmd rejected (security)`.
3. **OTA download truncation** — a transient TLS stall ended the download early and a
   partial image failed the SHA-256 check.

Each fix, plus the reconciliation and the repo→lib sync tooling, is detailed below.

**Reproduce a full OTA (local, self-signed HTTPS):**

1. Build an image whose version is a semver **≥ the device's current fw** (anti-downgrade);
   the device reports its compiled `NFF_FW_VERSION` after boot.
2. Put that exact `.bin` at `C:/data/firmware/<build_id>/firmware.elf` — the fleet SHA-256s
   *this file* for the signed command — and serve the **same bytes** at the firmware URL
   (the device downloads with `setInsecure()`, so any cert works).
3. `fleet_push_ota(device_id, "https://<host>:<port>/firmware.bin", build_id="<semver>")`.
4. Watch `ota_started` → device download → reboot → heartbeat shows the new fw. (After a USB
   hard-reflash, pulse-reset the device once so the broker drops its stale MQTT session, or
   commands won't be delivered.)

## Fix: enlarge MQTT buffer so OTA commands aren't dropped

**Problem:** OTA pushes via `fleet_push_ota` never reached devices running the
Arduino port. The signed OTA command (~421 B — it carries `version`, a 64-char
`sha256`, the `url`, and a ~144-char `cmd_sig`) exceeded **PubSubClient's default
256-byte buffer**. PubSubClient silently discards any inbound PUBLISH larger than
its buffer (`readPacket()` sets `len = 0`), so the receive callback never fired
and `nff_cmd_dispatch` never ran — no `ota_started`, no reboot, fw unchanged.
Smaller commands (`ping`/`diag` ≈ 216 B) fit, which is why those worked.

This had to be fixed in the SDK, not the fleet: the command can't be shrunk under
256 B (`cmd_sig` ~144 + `sha256` 64 = 208 B before any other field).

**Change:**

| File | Change |
|---|---|
| `include/nff.h` | Added tunable `NFF_MQTT_BUFFER_SIZE` (default `1024`, `#ifndef`-guarded, overridable via `-D`) |
| `src/port/nff_port_esp32_arduino.c` | `client.setBufferSize(NFF_MQTT_BUFFER_SIZE)` in `nff_port_mqtt_create` |
| `src/port/nff_port_esp8266_arduino.c` | Same one-liner (same PubSubClient default) |
| `src/port/nff_port_esp32_idf.c` | Set `cfg.buffer.size = NFF_MQTT_BUFFER_SIZE` (esp-mqtt already defaults to 1024; set explicitly for consistency) |

**Verification:** host build recompiles clean; all host tests pass
(`test_ota_rollback` 4/4, `test_cmd_dispatch` 5/5, `test_nonce_ring`). The new
`#define` is inert on the POSIX port; the `setBufferSize` calls live in
`ARDUINO`/`ESP_PLATFORM`-guarded code.

**To take effect on hardware:** rebuild the sketch (e.g. `hello_nff`) against this
patched SDK and reflash the device — existing 256-B firmware keeps dropping OTA
commands until reflashed. Reaching `committed` (vs just `ota_started`) also needs a
reachable HTTPS `firmware_url` whose served bytes match what the fleet hashes — both
now verified end-to-end (see **Status** above and the OTA-download fix below). The
ELF-vs-BIN hash question is sidestepped by serving the *same* `.bin` that is placed at
`firmware.elf`, so the fleet's hash and the device's hash agree.

## Fix: signature buffer off-by-one rejected ~26% of all signed commands

**Problem:** Even after the MQTT buffer fix, ~26% of `ping`/`reboot`/`diag`/`ota`
commands were rejected by the device with `cmd rejected (security)` — and it looked
random. It was **not** a key or signing problem: the fleet's command-signing public
key, the exported `cmd_verify.bin`, and the device's embedded `NFF_CMD_VERIFY_KEY_DER`
are byte-identical, the TBS canonical string matches on both sides, and ECDSA verifies
(`rc=0`) for every command that gets through.

Root cause: in `nff_security_verify_cmd`, the signature hex buffer was **one byte too
small** — `char sig_hex[144]`. A DER-encoded ECDSA-P256 signature is up to **72 bytes
= 144 hex chars**, which needs **145** bytes with the NUL terminator. When both `r` and
`s` have their high bit set (each gets a `0x00` sign pad) the signature is the full 72
bytes — measured at **~26% of signatures**. In that case `nff_json_get_str` can't fit
the value, returns -1, and `verify_cmd` rejects before ever reaching the crypto.
Sub-72-byte sigs (≤142 hex) fit, which is why ~74% worked and the failure looked
intermittent.

**Change:**

| File | Change |
|---|---|
| `src/nff_security.c` | `sig_hex[144]` → `sig_hex[145]` (144 hex chars + NUL) in `nff_security_verify_cmd` |

**Verification:** on-device instrumentation confirmed `ecdsa_rc=0` for 71-byte sigs
(genuine `pong`, ~600 ms RTT) and the reject path for the 72-byte case. After the fix,
**14/14 consecutive `fleet_ping` round-trips succeeded** (previously ~26% timed out).
This bug affected OTA too — the OTA command carries the same `cmd_sig` field — so it was
a second blocker to OTA on top of the MQTT buffer.

## Reconcile: the repo is now the single source of truth for the Arduino library

**Problem:** The Arduino IDE/CLI compiles a *separate, flattened copy* of this SDK at
`<sketchbook>/libraries/nff/`, not this repo. The two had drifted in both directions:
the repo had the MQTT buffer fix the deployed lib lacked, while the lib carried
Arduino-only fixes the repo lacked — so neither was a superset and a plain re-copy in
either direction would regress something.

**Change (merged the lib-only Arduino fixes back into the repo):**

| File | Change |
|---|---|
| `src/nff_crash.c` | Arduino-esp32 3.x lacks `esp_rom_printf` → use portable `printf`; add `#include "esp_system.h"` (both inside the shared `#if ESP_PLATFORM` block; `printf` is valid under ESP-IDF too) |
| `src/port/nff_port_esp32_arduino.c` | Adopt the lib's `der_to_pem()` TLS path (arduino-esp32 `WiFiClientSecure` needs PEM, not raw DER) and panic-handler removal (`esp_set_panic_handler` not exposed in arduino-esp32 3.x); the MQTT + signature buffer fixes ride along |

## New: automatic repo → Arduino library sync

So a repo change always reaches what sketches build against (no more drift):

| File | Purpose |
|---|---|
| `tools/sync_arduino_lib.py` | Regenerate the flat ESP32-only Arduino lib from the repo: duplicate `nff.h` to root + `src/`, rename the esp32 arduino port `.c`→`.cpp` (it is C++), exclude the esp8266/idf/posix ports, stamp `.nff_sync_meta`. Idempotent. |
| `tools/hooks/post-commit`, `tools/hooks/post-merge` | Run the generator on every commit/merge so the lib auto-syncs |
| `tools/install-hooks.ps1` | Install the hooks into `.git/hooks/` (not version-controlled; run once per clone) |

A companion `sketches/flash.ps1` (in the **nff-fleet** repo) does sync → `arduino-cli
compile -u` in one step, guaranteeing a reflash always picks up the latest repo sources
(including uncommitted edits).

## Fix: OTA download truncation silently passed as success → SHA-256 mismatch

**Problem:** With command delivery and signature verification fixed, the OTA reached
`ota_started` and the device began downloading — but aborted with `OTA SHA-256 mismatch`,
and the firmware server logged a mid-transfer connection reset. `nff_port_https_get_stream`
(esp32 arduino) broke out of its read loop on the **first transient empty read**
(`readBytes() <= 0` after the ~1 s stream timeout). A multi-hundred-KB TLS transfer
routinely stalls >1 s between records, so the loop exited early — returning `rc=0`
(*success*) on a **partial image**. `nff_ota.c` then hashed the truncated data → mismatch.

**Change:**

| File | Change |
|---|---|
| `src/port/nff_port_esp32_arduino.c` | Rewrote `nff_port_https_get_stream` to read against `Content-Length` (`http.getSize()`), drive reads from `stream->available()`, tolerate transient empty reads (only fail after a real >`timeout_ms` stall), and treat a short read as an error so a truncated image can never reach the SHA-256 check as if it succeeded |

**Verification:** end-to-end OTA now completes — `test-device-01` downloads the full
~1.05 MB image, SHA-256 matches, `Update.end()` switches the boot partition, the device
reboots into **fw=1.0.1**, and the fleet heartbeat reports `fw":"1.0.1"`. (Operational
note: after a USB hard-reflash the broker can briefly hold the device's old MQTT session —
heartbeats flow but commands don't until a clean reconnect; pulse-resetting the device
clears it.)
