#pragma once

#include <Arduino.h>

/**
 * WiStalker - passive 802.11 presence detection for the ESP8266 and ESP32.
 *
 * Puts the radio in promiscuous mode and reports, for every sniffed frame, the
 * layer-2 addresses it carries. Match those against the MAC addresses of the
 * devices you care about to know whether they are within radio range.
 *
 * Only the two addresses useful for presence are extracted - the receiver and,
 * when the frame carries it, the transmitter - so the hot receive path stays
 * tiny (no per-frame allocation, no full-frame copy). The 802.11 parsing is
 * defined by the IEEE standard, so it is shared; only the SDK glue differs
 * between the two chips and is selected at compile time.
 *
 * There is a single radio, hence a single detector: the class is not
 * instantiable and everything is called through it. Typical use:
 *
 *   void onFrame(void* ctx, const WiStalker::Frame& f) {
 *     // f.rx is always valid (MAC_LEN bytes); f.tx may be nullptr
 *   }
 *
 *   void setup() { WiStalker::begin(onFrame); WiStalker::start(); }
 *
 *   void loop() {
 *     for (uint8_t ch = WiStalker::CHANNEL_MIN; ch <= WiStalker::CHANNEL_MAX; ch++) {
 *       WiStalker::setChannel(ch);
 *       WiStalker::dwell(50);   // listen ~50 ms per channel
 *     }
 *   }
 *
 * Callback execution context - IMPORTANT, it differs by chip:
 *   - ESP8266: the callback runs cooperatively, from inside dwell()/yield() on
 *     the single core. It never preempts your loop() mid-statement.
 *   - ESP32:   the callback runs on the WiFi task, concurrently with loop().
 *     Any state you touch in BOTH the callback and loop() must be guarded
 *     (portMUX / critical section / std::atomic). Keep the callback short.
 *   Set things up with begin() before start(), and stop()/end() before
 *   reconfiguring; the callback only fires between start() and stop().
 *
 * Note: modern devices randomise their MAC address until they associate with an
 * access point, so a device is reliably seen only while it is connected to a
 * known network (using its real MAC) or has randomisation disabled.
 */
class WiStalker {
public:
    static constexpr uint8_t MAC_LEN     = 6;
    static constexpr uint8_t CHANNEL_MIN = 1;
    static constexpr uint8_t CHANNEL_MAX = 13;  // channels 12-14 are region-restricted

    /** A MAC address you can compare directly against the raw addresses handed
     *  to the frame callback, e.g. `if (watched == f.rx) ...`. */
    struct MacAddress {
        uint8_t bytes[MAC_LEN];

        bool operator==(const uint8_t* raw) const {
            for (uint8_t i = 0; i < MAC_LEN; i++) {
                if (bytes[i] != raw[i]) {
                    return false;
                }
            }
            return true;
        }
        bool operator!=(const uint8_t* raw) const { return !(*this == raw); }
    };

    /** A sniffed frame, reduced to what presence detection needs. The pointers
     *  are valid only for the duration of the callback - copy what you keep. */
    struct Frame {
        const uint8_t* rx;       // receiver (802.11 addr1), always valid (MAC_LEN bytes)
        const uint8_t* tx;       // transmitter (802.11 addr2), or nullptr (e.g. ACK/CTS)
        int8_t         rssi;     // signal strength, dBm
        uint8_t        channel;  // channel currently tuned (from setChannel)
    };

    /**
     * Frame callback.
     *   ctx   - the opaque pointer passed to begin(), so you can carry state
     *           without globals (nullptr if you did not pass one).
     *   frame - the sniffed frame (see Frame).
     */
    using Callback = void (*)(void* ctx, const Frame& frame);

    // Single radio -> single detector: not instantiable, call through the class.
    WiStalker() = delete;

    /** Enter promiscuous mode and register the callback. `ctx` is handed back to
     *  the callback untouched. The operating mode is set in RAM only, so this
     *  does not wear the RF/system-param flash. */
    static void begin(Callback cb, void* ctx = nullptr);

    /** Leave promiscuous mode so the radio can be used normally (STA/AP). */
    static void end();

    static void start();   // deliver sniffed frames to the callback
    static void stop();    // pause delivery (stays in promiscuous mode)

    static void setChannel(uint8_t channel);

    /** Yield to the SDK for `ms` milliseconds so queued frames get delivered to
     *  the callback. Call it after setChannel() to dwell on a channel. */
    static void dwell(uint32_t ms);
};
