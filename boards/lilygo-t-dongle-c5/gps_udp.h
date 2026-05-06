// UDP-NMEA GPS pump for the T-Dongle C5 wardriver build.
//
// Listens on a UDP port (bound to the AP interface) for NMEA-0183 sentences
// broadcast by the iPhone GPS2IP app, and feeds them into a caller-supplied
// TinyGPSPlus instance. Replaces the wired UART path used by Bruce's
// wardriving.cpp / gps_tracker.cpp on this board only.
//
// Compiled into the firmware only when LILYGO_T_DONGLE_C5 is defined.

#pragma once

#ifdef LILYGO_T_DONGLE_C5

#include <Arduino.h>
#include <TinyGPS++.h>
#include <WiFiUdp.h>

class UdpNmeaPump {
public:
    bool begin(uint16_t port = 11123);
    void end();

    // Drain any pending UDP datagrams into 'gps'. Call from the same loop()
    // that previously called gps.encode(GPSserial.read()).
    // Returns the number of bytes pushed this call.
    size_t feed(TinyGPSPlus &gps);

    bool isRunning() const { return _running; }
    uint32_t bytesPumped() const { return _totalBytes; }
    uint32_t lastDatagramMs() const { return _lastRxMs; }

private:
    WiFiUDP   _udp;
    bool      _running     = false;
    uint16_t  _port        = 0;
    uint32_t  _totalBytes  = 0;
    uint32_t  _lastRxMs    = 0;
};

extern UdpNmeaPump gpsUdpPump;

#endif // LILYGO_T_DONGLE_C5
