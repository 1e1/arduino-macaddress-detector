#include "WiStalker.hpp"

// ---------------------------------------------------------------------------
// Shared state and 802.11 parsing (chip-independent - defined by IEEE 802.11).
// The single radio means a single set of state, kept file-local so the public
// header exposes neither the state nor the internal dispatch helper.
// ---------------------------------------------------------------------------

namespace {

WiStalker::Callback s_cb      = nullptr;
void*                    s_ctx     = nullptr;
volatile bool            s_enabled = false;
volatile uint8_t         s_channel = WiStalker::CHANNEL_MIN;  // set by setChannel()

// 802.11 MAC header address offsets.
constexpr uint8_t ADDR1_OFFSET  = 4;    // receiver, present in every frame
constexpr uint8_t ADDR2_OFFSET  = 10;   // transmitter, present in mgmt & data frames

// Minimum frame length (bytes) for each address to actually be present, so a
// short/runt frame never has non-address bytes read as an address.
constexpr uint8_t MIN_LEN_ADDR1 = 10;   // FC(2) + duration(2) + addr1(6)
constexpr uint8_t MIN_LEN_ADDR2 = 16;   // + addr2(6)

// 802.11 frame types (bits 3-2 of the frame-control field).
constexpr uint8_t TYPE_MANAGEMENT = 0;
constexpr uint8_t TYPE_DATA       = 2;

// Shared receive core. `len` is the real frame length reported by the SDK; it
// gates the address reads so a truncated frame cannot leak non-address bytes.
void dispatch(const uint8_t* frame, int8_t rssi, uint16_t len) {
    if (!s_enabled || s_cb == nullptr) {
        return;
    }
    if (len < MIN_LEN_ADDR1) {
        return;   // too short to even carry the receiver address
    }

    // addr1 (receiver) is always present. addr2 (transmitter) is carried by
    // management and data frames long enough to hold it; control frames may not
    // have it, so report it only when meaningful.
    const uint8_t type  = (frame[0] >> 2) & 0x03;
    const bool    hasTx = (type == TYPE_MANAGEMENT || type == TYPE_DATA) && len >= MIN_LEN_ADDR2;
    const WiStalker::Frame f{
        frame + ADDR1_OFFSET,
        hasTx ? frame + ADDR2_OFFSET : nullptr,
        rssi,
        s_channel,   // reliable, unlike rx_ctrl.channel (often 0 on ESP8266 SDKs)
    };

    s_cb(s_ctx, f);
}

} // namespace

void WiStalker::start() { s_enabled = true; }
void WiStalker::stop()  { s_enabled = false; }

void WiStalker::dwell(uint32_t ms) {
    delay(ms);   // yields to the SDK, which delivers queued frames to the callback
}

// ===========================================================================
// ESP32 backend (ESP-IDF esp_wifi promiscuous API).
// ===========================================================================
#if defined(ARDUINO_ARCH_ESP32)

#include <WiFi.h>
#include <esp_wifi.h>

namespace {
void rxTrampoline(void* buf, wifi_promiscuous_pkt_type_t /*type*/) {
    const auto* pkt = static_cast<const wifi_promiscuous_pkt_t*>(buf);
    dispatch(pkt->payload, pkt->rx_ctrl.rssi, pkt->rx_ctrl.sig_len);
}
} // namespace

void WiStalker::begin(Callback cb, void* ctx) {
    s_cb  = cb;
    s_ctx = ctx;
    WiFi.mode(WIFI_STA);          // bootstraps and starts the WiFi stack
    WiFi.disconnect();            // stay unassociated; we only listen
    esp_wifi_set_promiscuous(false);
    const wifi_promiscuous_filter_t filter{
        WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA
    };
    esp_wifi_set_promiscuous_filter(&filter);   // control frames carry no useful TX/RX for us
    esp_wifi_set_promiscuous_rx_cb(&rxTrampoline);
    esp_wifi_set_promiscuous(true);
    s_enabled = false;
}

void WiStalker::end() {
    s_enabled = false;
    esp_wifi_set_promiscuous(false);
}

void WiStalker::setChannel(uint8_t channel) {
    s_channel = channel;
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

// ===========================================================================
// ESP8266 backend (Espressif NONOS SDK promiscuous API).
// ===========================================================================
#elif defined(ARDUINO_ARCH_ESP8266)

extern "C" {
  #include "user_interface.h"
}

namespace {

// ESP8266 promiscuous RX buffer layouts (Espressif NONOS SDK). The 802.11 frame
// header we care about lives in the `buf` field of the two frame-carrying ones;
// the SDK tells them apart by the callback's `len` argument. The full layout is
// kept so field offsets match the SDK ABI, even though we read only rssi + the
// frame length (channel comes from setChannel(), which is reliable).
struct RxControl {
    signed   rssi: 8;
    unsigned rate: 4;
    unsigned is_group: 1;
    unsigned: 1;
    unsigned sig_mode: 2;
    unsigned legacy_length: 12;
    unsigned damatch0: 1;
    unsigned damatch1: 1;
    unsigned bssidmatch0: 1;
    unsigned bssidmatch1: 1;
    unsigned MCS: 7;
    unsigned CWB: 1;
    unsigned HT_length: 16;
    unsigned Smoothing: 1;
    unsigned Not_Sounding: 1;
    unsigned: 1;
    unsigned Aggregation: 1;
    unsigned STBC: 2;
    unsigned FEC_CODING: 1;
    unsigned SGI: 1;
    unsigned rxend_state: 8;
    unsigned ampdu_cnt: 8;
    unsigned channel: 4;
    unsigned: 12;
};

struct LenSeq {
    uint16_t length;
    uint16_t seq;
    uint8_t  address3[6];
};

struct sniffer_buf {
    RxControl rx_ctrl;
    uint8_t   buf[36];
    uint16_t  cnt;
    LenSeq    lenseq[1];
};

struct sniffer_buf2 {
    RxControl rx_ctrl;
    uint8_t   buf[112];
    uint16_t  cnt;
    uint16_t  len;
};

void rxTrampoline(uint8_t* buf, uint16_t len) {
    const uint8_t* frame;
    int8_t         rssi;
    uint16_t       frameLen;

    if (len == sizeof(sniffer_buf2)) {
        const auto* s = reinterpret_cast<const sniffer_buf2*>(buf);
        frame    = s->buf;
        rssi     = s->rx_ctrl.rssi;
        frameLen = s->len;
    } else if (len == sizeof(RxControl)) {
        return;   // control info only, no MAC header to read
    } else {
        const auto* s = reinterpret_cast<const sniffer_buf*>(buf);
        frame    = s->buf;
        rssi     = s->rx_ctrl.rssi;
        frameLen = s->lenseq[0].length;
    }

    dispatch(frame, rssi, frameLen);
}

} // namespace

void WiStalker::begin(Callback cb, void* ctx) {
    s_cb  = cb;
    s_ctx = ctx;
    wifi_station_disconnect();
    wifi_set_opmode_current(STATION_MODE);   // RAM only: no RF/system-param flash write
    wifi_promiscuous_enable(false);
    wifi_set_promiscuous_rx_cb(&rxTrampoline);
    wifi_promiscuous_enable(true);
    s_enabled = false;
}

void WiStalker::end() {
    s_enabled = false;
    wifi_promiscuous_enable(false);
}

void WiStalker::setChannel(uint8_t channel) {
    s_channel = channel;
    wifi_set_channel(channel);
}

// ===========================================================================
#else
#error "WiStalker targets the ESP8266 and ESP32 only (needs a promiscuous-mode WiFi SDK)."
#endif
