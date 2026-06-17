# Testing OTA on ESP32

This guide walks through every level of OTA testing: host unit tests (no hardware), a full device upgrade, error injection, and the rollback path.

---

## Table of contents

1. [Prerequisites](#prerequisites)
2. [Level 1 — Host unit tests (no hardware)](#level-1--host-unit-tests-no-hardware)
3. [Level 2 — Full device OTA](#level-2--full-device-ota)
   - [Step 1: Build new firmware](#step-1-build-new-firmware)
   - [Step 2: Host the binary over HTTPS](#step-2-host-the-binary-over-https)
   - [Step 3: Verify the device is online](#step-3-verify-the-device-is-online)
   - [Step 4: Send the OTA command](#step-4-send-the-ota-command)
   - [Step 5: Monitor progress](#step-5-monitor-progress)
   - [Step 6: Verify committed result](#step-6-verify-committed-result)
4. [Level 3 — Error scenarios](#level-3--error-scenarios)
5. [Level 4 — Rollback path](#level-4--rollback-path)
6. [Appendix A: Partition table requirements](#appendix-a-partition-table-requirements)
7. [Appendix B: NVS keys reference](#appendix-b-nvs-keys-reference)
8. [Appendix C: Full OTA flow diagram](#appendix-c-full-ota-flow-diagram)

---

## Prerequisites

### Hardware
- ESP32 board flashed with a firmware that includes the nff SDK
- Partition table with at least two OTA partitions (see [Appendix A](#appendix-a-partition-table-requirements))

### Services
- `nff-fleet` broker running locally or in the cloud
- Device provisioned: `credentials.h` generated via `nff provision new-device --id <id>`
- Device connected to WiFi and MQTT (confirmed by a heartbeat on `nff/devices/<id>/heartbeat`)

### Tools
| Tool | Purpose |
|---|---|
| `idf.py` | ESP-IDF build and flash |
| `certutil` (Windows) / `sha256sum` (Linux/macOS) | Compute firmware SHA-256 |
| `mosquitto_sub` | Watch MQTT topics during testing |
| Claude + nff-fleet MCP | Send signed OTA commands (recommended) |

---

## Level 1 — Host unit tests (no hardware)

Run these first. They execute fully on your development machine and cover the four core OTA scenarios without any device or network.

### Build

```cmd
cd nff-sdk-c
cmake -B build -G "MinGW Makefiles"
cmake --build build
```

### Run

```cmd
cmd /c build\tests\test_ota_rollback.exe
```

Expected output:

```
PASS: pending committed OTA result published
PASS: pending rolled_back OTA result published
PASS: no pending OTA → nothing published
PASS: OTA downgrade rejected
All OTA rollback tests passed.
```

### What each test covers

| Test | Simulated scenario |
|---|---|
| `pending committed` | NVS contains `ota_pending=1, ota_committed=1` on boot → device publishes `status=committed` after reconnect |
| `pending rolled_back` | NVS contains `ota_pending=1, ota_committed=0` on boot → device publishes `status=rolled_back` |
| `no pending no publish` | Clean boot (no NVS keys) → nothing published, no false positive |
| `downgrade rejected` | OTA command targets `v1.0.0` while device is at `v2.0.0` → error response, no download attempted |
| `trial commit after soak` | Healthy trial image is committed only once it holds healthy for the soak window (`NFF_OTA_MIN_HEALTHY_MS`), not on the first tick |
| `heap floor veto` | `min_free_heap` below `NFF_OTA_MIN_HEAP_FLOOR` vetoes commit; the image is rolled back at the confirm deadline |

Source: `tests/test_ota_rollback.c`

### Resumable download + crash backtrace (V1.2)

```cmd
cmd /c build\tests\test_ota_resume.exe
cmd /c build\tests\test_crash_report.exe
```

| Test | Simulated scenario |
|---|---|
| `resume after drop` (`test_ota_resume.c`) | The HTTPS mock drops mid-stream once; the device resumes via `Range` from the offset and the reassembled image matches the SHA-256 in the signed command → trial armed + reboot |
| `resume unsupported restart` (`test_ota_resume.c`) | The mock answers a resume request with `200` (ignored Range); the device restarts the download clean (no double-write) and still verifies |
| `crash report backtrace` (`test_crash_report.c`) | A staged coredump summary + `crash_simulate` boot → retained crash payload carries `pc`/`exception_cause`/`backtrace[]`/`rtc_log`/`fw_version`/`build_id`; `crash_simulate` is erased |
| `no crash no report` (`test_crash_report.c`) | Clean boot publishes nothing on the crash topic |

> Note: build host tests with **GCC/MinGW**, not MSVC — the POSIX port uses `__attribute__((weak))`
> for the overridable ECDSA stub, which MSVC rejects.

---

## Level 2 — Full device OTA

### Step 1: Build new firmware

Bump `NFF_FW_VERSION` to a version **strictly higher** than what is currently running (the anti-downgrade check uses semantic versioning). For example, if the device is at `1.0.0`, use `2.0.0`.

**ESP-IDF:** Edit `credentials.h` or pass via build flag:

```cmake
# In your CMakeLists.txt or via command line
idf.py build -D NFF_FW_VERSION='"2.0.0"'
```

**Arduino/PlatformIO:** Edit `credentials.h` directly:

```c
#define NFF_FW_VERSION "2.0.0"
```

Run the build:

```cmd
idf.py build
```

The binary is at `build/<project-name>.bin` (ESP-IDF) or `.pio/build/<env>/firmware.bin` (PlatformIO).

### Compute SHA-256

The exact SHA-256 of the `.bin` file (not the ELF) must be sent in the OTA command.

**Windows:**
```cmd
certutil -hashfile build\my-project.bin SHA256
```

**Linux / macOS:**
```bash
sha256sum build/my-project.bin
```

Save the 64-character hex string — you will need it in Step 4.

---

### Step 2: Host the binary over HTTPS

The ESP32 `esp_http_client` requires a valid TLS certificate. Plain HTTP will fail.

**Option A — ngrok (quickest for local dev)**

```bash
python -m http.server 8000
# in a second terminal:
ngrok http 8000
```

ngrok gives you a public `https://` URL. The binary is reachable at `https://<ngrok-subdomain>.ngrok.io/my-project.bin`.

The URL is valid until you stop ngrok. Make sure to send the OTA command before the session expires.

**Option B — Cloud storage (recommended for real testing)**

Upload the `.bin` to S3 or Cloudflare R2, then generate a pre-signed URL. The URL must remain valid for at least the duration of the download (5-minute timeout in the SDK). A 30-minute expiry is a safe margin.

---

### Step 3: Verify the device is online

Subscribe to the heartbeat topic and confirm `fw_version` matches your baseline before the upgrade:

```bash
mosquitto_sub -h <broker-host> -p 8883 \
  --cert client.crt --key client.key --cafile ca.crt \
  -t "nff/devices/<device-id>/heartbeat"
```

The retained heartbeat message looks like:

```json
{"status":"online","id":"my-device","fw":"1.0.0","build":"aabbccdd11223344","heap":180000,"uptime":42}
```

Confirm `"fw":"1.0.0"` matches your current firmware before proceeding.

---

### Step 4: Send the OTA command

**Recommended path — via Claude + nff-fleet MCP**

Ask Claude to call `fleet_push_ota`. The tool handles nonce generation, timestamp, and ECDSA-P256 command signing automatically:

```
fleet_push_ota(
  device_id="my-device",
  firmware_url="https://<your-hosting-url>/my-project.bin",
  build_id="2.0.0",
  confirmed=False
)
```

The tool blocks for up to 120 seconds and returns the device's `ota_result` response as soon as it arrives.

**Manual path — raw MQTT publish**

Only practical if you have access to the fleet signing key to produce a valid `cmd_sig`. The command payload must be:

```json
{
  "action":    "ota",
  "version":   "2.0.0",
  "url":       "https://<your-hosting-url>/my-project.bin",
  "sha256":    "<64-char hex>",
  "size":      <byte-count>,
  "nonce":     "<8 random hex chars>",
  "timestamp": <unix epoch seconds>,
  "cmd_sig":   "<ECDSA-P256 DER signature over 'ota|version|sha256|nonce|timestamp', hex-encoded>"
}
```

Without a valid `cmd_sig` the device will silently drop the command (signature check happens before any OTA logic).

---

### Step 5: Monitor progress

Open the response topic in a second terminal:

```bash
mosquitto_sub -h <broker-host> -p 8883 \
  --cert client.crt --key client.key --cafile ca.crt \
  -t "nff/devices/<device-id>/response"
```

Also open a serial monitor (`idf.py monitor` or equivalent) to see SDK log output.

**Timeline of expected messages:**

| When | MQTT message | Serial log |
|---|---|---|
| Command received | `{"type":"ota_started","version":"2.0.0","id":"my-device"}` | `nff: OTA start v2.0.0` |
| Download in progress | *(none)* | Periodic progress logs |
| Download complete | *(none)* | `nff: OTA download complete, rebooting` |
| ~500 ms later | *(device reboots — MQTT disconnect)* | Device resets |
| After reconnect | `{"type":"ota_result","status":"committed","version":"2.0.0","id":"my-device"}` | `nff: OTA start` (not shown — new firmware) |

The download can take several minutes on a slow connection. The SDK uses a 5-minute timeout (`300 000 ms`). No heartbeats are sent during the download (`NFF_STATE_OTA_ACTIVE` blocks the heartbeat timer).

---

### Step 6: Verify committed result

Checklist:

- [ ] `ota_result` message received with `"status":"committed"` and `"version":"2.0.0"`
- [ ] New heartbeat appears with `"fw":"2.0.0"` after reconnect
- [ ] Manual reboot of the device does **not** re-publish `ota_result` (NVS keys are cleared on first read — see `nff_core.c:70–81`)
- [ ] Serial monitor shows the new firmware version in the boot log

---

## Level 3 — Error scenarios

Each scenario verifies a specific guard in `src/nff_ota.c`. Check `nff/devices/<id>/response` for the error JSON after each.

### Anti-downgrade rejection

Send an OTA command targeting a version **lower than or equal to** the current firmware. For a device at `v2.0.0`:

```
fleet_push_ota(device_id="my-device", firmware_url="...", build_id="1.0.0")
```

Expected response:

```json
{"type":"error","msg":"ota: downgrade rejected","current":"2.0.0","requested":"1.0.0"}
```

No download is attempted. Device remains at `NFF_STATE_CONNECTED`.

### SHA-256 mismatch

Pass the correct firmware URL but deliberately corrupt the `sha256` parameter (flip one hex character). Via `fleet_push_ota` you can pass the sha256 directly:

```
fleet_push_ota(
  device_id="my-device",
  firmware_url="...",
  build_id="2.0.0",
  sha256_override="0000000000000000000000000000000000000000000000000000000000000000"
)
```

*(If the MCP tool doesn't expose `sha256_override`, upload a different binary at the same URL with a mismatched hash.)*

Expected response:

```json
{"type":"error","msg":"ota: sha256 mismatch"}
```

The OTA partition is aborted (`esp_ota_abort`). The previous firmware remains active.

### Unreachable URL

Use a URL that returns 404 or is unreachable:

```
fleet_push_ota(device_id="my-device", firmware_url="https://invalid.example.com/notfound.bin", build_id="2.0.0", ...)
```

Expected response:

```json
{"type":"error","msg":"ota: download failed"}
```

### Partition begin failure

This is triggered if no valid OTA partition exists in the partition table. Not easy to inject at runtime — verifiable via the host unit test layer or by deliberately using a partition table with only a `factory` partition and no `ota_0`/`ota_1`. In that case `esp_ota_get_next_update_partition()` returns `NULL` and the SDK publishes:

```json
{"type":"error","msg":"ota: begin failed"}
```

---

## Level 4 — Rollback path

### How ESP32 OTA rollback works

When `esp_ota_set_boot_partition()` is called (inside `nff_port_ota_end`), ESP-IDF marks the new partition as the active boot target. If `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is set in your `sdkconfig`, the bootloader additionally marks the partition as **pending verification**. The new firmware must then call `esp_ota_mark_app_valid_cancel_rollback()` to confirm it. If it crashes before doing so, the next reset causes the bootloader to mark the partition as invalid and revert to the previous one.

### V1 SDK note: `ota_committed` flag is optimistic

nff-sdk-c writes `ota_committed=1` to NVS **before** rebooting into the new firmware (see `nff_ota.c:197`). This means:

- If the new firmware boots and reconnects normally → `ota_result=committed` is correct.
- If the new firmware crashes and the **bootloader rolls back to the previous partition** → the old firmware boots, reads `ota_committed=1` from NVS, and incorrectly reports `ota_result=committed`.

To get an accurate `ota_result=rolled_back`, the new firmware must explicitly write `ota_committed=0` to NVS **before** it crashes:

```c
// In the new firmware, before the panic/abort that triggers rollback:
nff_port_nvs_set_str("ota_committed", "0");
nff_port_nvs_commit();
// ... crash / abort / assert here ...
```

### Simulating a rollback

1. Build a firmware (v2.0.0) that writes `ota_committed=0` and then calls `abort()` early in `app_main`, before `nff_connect()`:

```c
void app_main(void) {
    nvs_flash_init();
    // ... WiFi init ...
    nff_init(&cfg);  // reads OTA NVS flags, sets pending_ota_result

    // Simulate bad firmware: mark rollback and crash
    nff_port_nvs_set_str("ota_committed", "0");
    nff_port_nvs_commit();
    abort();  // triggers bootloader rollback (with ROLLBACK_ENABLE=y)
}
```

2. OTA this firmware in via `fleet_push_ota`.

3. After the device crashes and the bootloader reverts to `v1.0.0`:
   - Old firmware boots, calls `nff_init()`, reads `ota_pending=1, ota_committed=0`
   - After `nff_connect()`, publishes:

```json
{"type":"ota_result","status":"rolled_back","version":"2.0.0","id":"my-device"}
```

4. Check that the heartbeat now shows `"fw":"1.0.0"` again.

### Enabling bootloader rollback in ESP-IDF

In your `sdkconfig` (or `menuconfig`):

```
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```

Without this setting the bootloader will not automatically revert partitions on crash — your test firmware would need to call `esp_ota_set_boot_partition()` manually to switch back.

---

## Appendix A: Partition table requirements

OTA requires at least two OTA partitions. The standard ESP-IDF dual-OTA partition table is a good starting point:

```
# partitions_two_ota.csv
# Name,   Type, SubType, Offset,  Size,  Flags
nvs,      data, nvs,     0x9000,  0x4000,
otadata,  data, ota,     0xd000,  0x2000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 1M,
ota_0,    app,  ota_0,   ,        1M,
ota_1,    app,  ota_1,   ,        1M,
```

Use in your project:

```cmake
# CMakeLists.txt or sdkconfig
set(PARTITION_TABLE_FILE "partitions_two_ota.csv")
```

**Sizing:** Each OTA partition must be at least as large as your firmware binary. A WiFi + TLS + nff SDK build is typically 1.2–1.5 MB. If the binary doesn't fit, `esp_ota_begin()` returns an error and the SDK publishes `ota: begin failed`.

---

## Appendix B: NVS keys reference

All keys are stored in namespace `"nff"` (see `nff_port_esp32_idf.c: nvs_open("nff", ...)`).

| Key | Value | Written by | Read by | Cleared by |
|---|---|---|---|---|
| `ota_pending` | `"1"` | `nff_ota.c` before reboot | `nff_core.c` on init | `nff_core.c` on init |
| `ota_version` | e.g. `"2.0.0"` | `nff_ota.c` before reboot | `nff_core.c` on init | `nff_core.c` on init |
| `ota_committed` | `"1"` (success) or `"0"` (rolled back) | `nff_ota.c` writes `"1"`; app must write `"0"` for rollback | `nff_core.c` on init | `nff_core.c` on init |

All three keys are erased atomically during `nff_init()` after being read, so `ota_result` is only ever published once per OTA event.

---

## Appendix C: Full OTA flow diagram

```
                   nff-fleet (server)              Device (ESP32)
                   ─────────────────               ──────────────
fleet_push_ota()
        │
        ├─ compute nonce, timestamp
        ├─ sign TBS: "ota|ver|sha256|nonce|ts"
        │
        └─► MQTT publish ──────────────────────► nff/devices/<id>/cmd
                                                         │
                                                  verify cmd_sig (ECDSA-P256)
                                                         │ OK
                                                  anti-downgrade check
                                                         │ OK
                                                  publish ota_started
                        nff/devices/<id>/response ◄──────┤
                                                         │
                                                  state = OTA_ACTIVE
                                                  esp_ota_begin()
                                                         │
                                                  HTTPS GET ──► CDN
                                                         │ chunks
                                                  sha256_update (mbedTLS)
                                                  esp_ota_write()
                                                         │
                                                  sha256_finish()
                                                  memcmp(actual, expected)
                                                         │ OK
                                                  esp_ota_end()
                                                  esp_ota_set_boot_partition()
                                                         │
                                                  NVS: ota_pending=1
                                                        ota_version=<ver>
                                                        ota_committed=1
                                                         │
                                                  delay 500ms → esp_restart()
                                                         │
                                                  ═══════╪══ REBOOT ══════════
                                                         │
                                                  nff_init() reads NVS:
                                                    pending_ota_result = true
                                                    NVS keys erased
                                                         │
                                                  nff_connect() → MQTT up
                                                         │
                                                  nff_ota_check_pending_result()
                                                         │
                                                  publish ota_result
                        nff/devices/<id>/response ◄──────┘
                        {"type":"ota_result",
                         "status":"committed",
                         "version":"2.0.0",
                         "id":"my-device"}
```
