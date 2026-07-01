# Development notes

Context and conventions for anyone working on this library.

## What this is

`WiStalker` — an ESP8266/ESP32 library for passive 802.11 presence
detection (promiscuous sniffing). It was extracted from the
**arduino-macaddress-notifier** project ("the notifier"), which is its primary
consumer: the notifier watches for known MAC addresses and fires rules/webhooks.
Keep that use case in mind.

## Build target

- **ESP8266 and ESP32.** One public API, one shared 802.11 parser, two
  compile-time backends selected by `ARDUINO_ARCH_*`.
  - ESP8266: FQBN `esp8266:esp8266:d1_mini`, core `esp8266:esp8266@3.1.2`
    (pinned in CI). NONOS SDK promiscuous API from `user_interface.h`:
    `wifi_set_opmode_current`, `wifi_promiscuous_enable`,
    `wifi_set_promiscuous_rx_cb`, `wifi_set_channel`.
  - ESP32: FQBN `esp32:esp32:esp32`. ESP-IDF `esp_wifi` API:
    `esp_wifi_set_promiscuous`, `esp_wifi_set_promiscuous_filter`,
    `esp_wifi_set_promiscuous_rx_cb`, `esp_wifi_set_channel`.

## Public API contract — keep it stable

The notifier depends on exactly this surface. Optimise the internals freely, but
do not change these signatures/semantics without a **major** version bump and a
coordinated update of the notifier:

```cpp
class WiStalker {
  struct MacAddress { uint8_t bytes[MAC_LEN]; /* ==, != against const uint8_t* */ };
  struct Frame { const uint8_t* rx; const uint8_t* tx; int8_t rssi; uint8_t channel; };
  using Callback = void (*)(void* ctx, const Frame& frame);
  WiStalker() = delete;                        // single radio -> call through the class
  static void begin(Callback cb, void* ctx = nullptr);  // enter promiscuous, register cb
  static void end();                                // leave promiscuous
  static void start();  static void stop();
  static void setChannel(uint8_t channel);
  static void dwell(uint32_t ms);                   // yield so queued frames get delivered
  static constexpr uint8_t MAC_LEN = 6, CHANNEL_MIN = 1, CHANNEL_MAX = 13;
};
```

Callback contract: `f.rx` always valid (`MAC_LEN` bytes); `f.tx` may be `nullptr`
(frames without a transmitter address, e.g. ACK/CTS, or frames too short to
carry one); `f.rssi` in dBm; `f.channel` is the tuned channel (from
`setChannel()`).

## Callback execution context

This differs by chip and any consumer must account for it:

- **ESP8266** — cooperative: the callback runs from inside `dwell()`/`yield()` on
  the single core; it never preempts `loop()` mid-statement.
- **ESP32** — concurrent: the callback runs on the WiFi task, in parallel with
  `loop()`. State shared between the callback and `loop()` must be guarded
  (portMUX / critical section / atomic).

`s_cb`/`s_ctx` are plain (not atomic): safe because they are set in `begin()`
before `start()`, and the callback is gated by the `volatile s_enabled` flag.
Preserve that ordering rather than reconfiguring while running.

## Invariants to preserve (they come from a review of the original code)

The pre-extraction code had real defects; the current version fixes them. Do not
regress:

- **No per-frame heap or large stack allocations in the receive callback.** The
  old code placed a ~590-byte struct (incl. a 512-byte raw copy) on the stack on
  every frame — never reintroduce that. Extract only `addr1` (offset 4) and, for
  management/data frames, `addr2` (offset 10). No SSID parse, no seq number, no
  full-frame copy.
- **Length-checked address reads.** Never read `addr1` unless the frame length is
  ≥ 10, nor `addr2` (offset 10) unless it is ≥ 16 — a runt frame must not have
  non-address bytes read as an address. Both backends provide the length
  (`sig_len` on ESP32, `len`/`lenseq[0].length` on ESP8266).
- **Report `setChannel()`'s channel, not `rx_ctrl.channel`.** The SDK's per-frame
  channel is unreliable (often 0) on some ESP8266 SDKs; the tuned channel
  (`s_channel`) is reliable and consistent across both chips.
- **ESP32 promiscuous filter is restricted to mgmt+data.** Control frames carry
  nothing useful for presence; keep the explicit filter.
- **RAM-only operating mode (ESP8266):** `wifi_set_opmode_current`, never
  `wifi_set_opmode` (that one writes the RF/system-param flash sectors → wear).
- Shared callback/loop state is `volatile` (`s_enabled`, `s_channel`).
- Header guarded (`#pragma once`); declarations in `.hpp`, definitions in `.cpp`.
  No file-scope variables/functions in the header — the mutable state and the
  internal `dispatch` helper live in the `.cpp` anonymous namespace, so the
  public API exposes neither.
- Scan channels 1–13 only (12–14 are region-restricted).

## Optimisation directions

- Optionally recover `addr2` for RTS control frames (it carries a valid TA)
  without matching non-address bytes on ACK/CTS.
- Optional RSSI threshold / short-term de-duplication to cut callback churn.
- Keep the receive path allocation-free and short — it runs in the SDK context
  (and concurrently on ESP32).

## Known limitation

Modern devices randomise their MAC until they associate with an access point, so
a device is reliably detected only while connected to a known network (real MAC)
or with randomisation disabled. Document it; do not try to "fix" it.

## Conventions

- **Keep the project neutral.** No generator/tool attribution anywhere — no
  co-author trailers, no "generated with" lines, no signatures — in code,
  comments, docs, or commit messages.
- Commit messages: English, imperative, factual.
- Git identity for this repo: `1e1`.
- Code style: 4-space indent, doc comments on the public API, match the existing
  files.

## Verify before committing

```sh
arduino-cli compile --fqbn esp8266:esp8266:d1_mini --library . examples/presence
arduino-cli compile --fqbn esp32:esp32:esp32       --library . examples/presence
```

CI (`.github/workflows/compile-examples.yml`) runs both on every push, plus
`arduino-lint`. For the integration check, the notifier compiles against this
repo with `arduino-cli compile --library <path> sketch_wiscan`, and its CI
installs the library by git tag.

## Versioning

Keep the public API stable, so optimisation work lands as patch/minor releases
(`1.0.x` / `1.x.0`). Tag each release; the notifier pins the library by tag in
its CI, then bumps the pin to adopt a new release.
