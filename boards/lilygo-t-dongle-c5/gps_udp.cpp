#ifdef LILYGO_T_DONGLE_C5

#include "gps_udp.h"

UdpNmeaPump gpsUdpPump;

bool UdpNmeaPump::begin(uint16_t port) {
    if (_running) return true;
    if (!_udp.begin(port)) {
        Serial.printf("[gps_udp] bind to UDP :%u failed\n", (unsigned)port);
        return false;
    }
    _port    = port;
    _running = true;
    Serial.printf("[gps_udp] listening on UDP :%u for NMEA broadcasts\n", (unsigned)port);
    return true;
}

void UdpNmeaPump::end() {
    if (!_running) return;
    _udp.stop();
    _running = false;
}

size_t UdpNmeaPump::feed(TinyGPSPlus &gps) {
    if (!_running) return 0;

    size_t pushed = 0;
    // Drain every queued datagram in this call so we don't fall behind GPS2IP's
    // 1 Hz cadence even if loop() runs slowly.
    while (true) {
        int sz = _udp.parsePacket();
        if (sz <= 0) break;

        // 256 bytes is the longest single NMEA sentence + CRLF; oversized
        // datagrams get truncated rather than overrunning the stack.
        uint8_t buf[256];
        int n = _udp.read(buf, sizeof(buf));
        if (n <= 0) continue;

        for (int i = 0; i < n; i++) gps.encode((char)buf[i]);
        pushed      += (size_t)n;
        _totalBytes += (uint32_t)n;
        _lastRxMs    = millis();
    }
    return pushed;
}

#endif // LILYGO_T_DONGLE_C5
