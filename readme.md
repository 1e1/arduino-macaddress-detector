# WiStalker

Passive 802.11 presence detection for the ESP8266 and ESP32.

It puts the radio in promiscuous mode and reports, for every sniffed frame, the
layer-2 addresses it carries. Match those against the MAC addresses of the
devices you care about to know whether they are within radio range — without
associating to anything or transmitting a single packet.

## How it works

The WiFi SDK hands each sniffed frame to a receive callback. WiStalker
extracts only what presence detection needs:

- **addr1 (receiver)** — always present.
- **addr2 (transmitter)** — present on management and data frames; reported as
  `nullptr` otherwise (e.g. ACK/CTS control frames), so you never match against
  non-address bytes.

Nothing else is parsed or copied, so the receive path stays small and cheap —
important because it runs in the SDK's context.

The 802.11 frame layout is defined by the IEEE standard, not by the chip, so the
parsing is shared. Only the SDK glue differs and is selected at compile time:
the Espressif NONOS SDK on ESP8266, ESP-IDF (`esp_wifi`) on ESP32. Same API on
both.

## Usage

```cpp
#include <WiStalker.hpp>

// Watch state, passed to the callback via ctx (no globals needed there).
struct Watch { WiStalker::MacAddress mac; bool seen; };
Watch watched = {{{ 0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33 }}, false};

void onFrame(void* ctx, const WiStalker::Frame& f) {
  Watch* w = static_cast<Watch*>(ctx);
  if (w->mac == f.rx || (f.tx && w->mac == f.tx)) {
    w->seen = true;   // f.rssi and f.channel are also available
  }
}

void setup() {
  WiStalker::begin(onFrame, &watched);
  WiStalker::start();
}

void loop() {
  for (uint8_t ch = WiStalker::CHANNEL_MIN; ch <= WiStalker::CHANNEL_MAX; ch++) {
    WiStalker::setChannel(ch);
    WiStalker::dwell(50);   // listen ~50 ms per channel
  }
}
```

There is a single radio, so `WiStalker` is not instantiable — call
everything through the class. To use the radio normally (connect as a station,
run an access point), call `WiStalker::end()` to leave promiscuous mode,
then `begin()` + `start()` to resume sniffing afterwards.

## API

| Call | Purpose |
| --- | --- |
| `begin(cb, ctx = nullptr)` | Enter promiscuous mode; register the callback. `ctx` is handed back to the callback untouched. |
| `end()` | Leave promiscuous mode so the radio can be used normally. |
| `start()` / `stop()` | Open / pause frame delivery to the callback (stays in promiscuous mode). |
| `setChannel(ch)` | Tune the radio to one channel (`CHANNEL_MIN`..`CHANNEL_MAX`). |
| `dwell(ms)` | Yield for `ms` so the SDK can deliver queued frames to the callback. |

The callback receives a `Frame` (`rx`, `tx`, `rssi`, `channel`); `MacAddress`
compares directly against `rx`/`tx` (`watched == f.rx`).

## Caveats

- **ESP8266 & ESP32 only.** It relies on each chip's promiscuous WiFi SDK API.
- **MAC randomisation.** Modern phones and laptops randomise their MAC address
  until they associate with an access point. A device is therefore reliably
  detected only while it is connected to a known network (using its real MAC) or
  has randomisation disabled.
- **Channel hopping is up to you.** The radio hears one channel at a time; loop
  over `CHANNEL_MIN..CHANNEL_MAX` and `dwell()` on each, as shown above. Channels
  12–14 are omitted as they are region-restricted and rarely used.

## Origins & license

The promiscuous receive path derives from the well-known ESPPL ("ESP Promiscuous
Packet Logger") sniffer approach, rewritten and trimmed to a minimal
presence-detection API. Released under the MIT License — see [LICENSE](LICENSE).
