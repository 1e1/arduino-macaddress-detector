#include <Arduino.h>

#include <WiStalker.hpp>


// State handed to the callback through the ctx pointer - no globals needed there.
struct Watch {
    WiStalker::MacAddress mac;
    bool                       seen;
};

// MAC address of a device to watch for (replace with a real one).
Watch watched = {
    {{ 0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33 }},   // .mac
    false,                                       // .seen
};


void onFrame(void* ctx, const WiStalker::Frame& f) {
    Watch* w = static_cast<Watch*>(ctx);
    if (w->mac == f.rx || (f.tx && w->mac == f.tx)) {
        w->seen = true;
        Serial.printf("watched device seen on ch %u (rssi %d)\n", (unsigned)f.channel, f.rssi);
    }
}

void setup() {
    Serial.begin(115200);
    WiStalker::begin(onFrame, &watched);
    WiStalker::start();
}

void loop() {
    for (uint8_t ch = WiStalker::CHANNEL_MIN; ch <= WiStalker::CHANNEL_MAX; ch++) {
        WiStalker::setChannel(ch);
        WiStalker::dwell(50);   // listen ~50 ms per channel; frames arrive via onFrame()
    }
}
