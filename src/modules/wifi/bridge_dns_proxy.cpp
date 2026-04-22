#include "bridge_dns_proxy.h"

#include <Arduino.h>
#include <esp_random.h>

#include <cstring>

namespace {

constexpr size_t kDnsHeaderSize = 12;
constexpr size_t kMaxDnsPacket = 512;
constexpr size_t kMaxDnsReply = 1024;

}  // namespace

BridgeDnsProxy::BridgeDnsProxy(std::set<uint32_t> &authedIps) : _authedIps(authedIps) {}

BridgeDnsProxy::~BridgeDnsProxy() { stop(); }

bool BridgeDnsProxy::start(IPAddress apIp) {
    if (_running) return true;
    _apIp = apIp;

    if (!_server.listen(apIp, 53)) {
        Serial.println("[DNSPROXY] listen on :53 failed");
        return false;
    }
    _server.onPacket([this](AsyncUDPPacket pkt) { onIncomingQuery(pkt); });

    if (!_upstream.connect(_upstreamDns, 53)) {
        Serial.printf(
            "[DNSPROXY] upstream connect to %s:53 failed\n", _upstreamDns.toString().c_str()
        );
        _server.close();
        return false;
    }
    _upstream.onPacket([this](AsyncUDPPacket reply) { onUpstreamReply(reply); });

    _running = true;
    _lastSweep = millis();
    Serial.printf(
        "[DNSPROXY] started on %s:53 upstream=%s\n",
        _apIp.toString().c_str(),
        _upstreamDns.toString().c_str()
    );
    return true;
}

void BridgeDnsProxy::stop() {
    if (!_running) return;
    _server.close();
    _upstream.close();
    _inflight.clear();
    _running = false;
    Serial.println("[DNSPROXY] stopped");
}

uint16_t BridgeDnsProxy::allocTxid() {
    for (int i = 0; i < 16; i++) {
        uint16_t candidate = (uint16_t)(esp_random() & 0xFFFF);
        if (candidate == 0) candidate = 1;
        if (_inflight.find(candidate) == _inflight.end()) return candidate;
    }
    return (uint16_t)((esp_random() & 0xFFFE) | 1);
}

void BridgeDnsProxy::onIncomingQuery(AsyncUDPPacket &pkt) {
    if (pkt.length() < kDnsHeaderSize) return;
    uint32_t clientIp = (uint32_t)pkt.remoteIP();
    bool authed = _authedIps.find(clientIp) != _authedIps.end();
    if (authed) forwardUpstream(pkt);
    else sendHijackReply(pkt);
    sweep(millis());
}

void BridgeDnsProxy::sendHijackReply(AsyncUDPPacket &query) {
    size_t qlen = query.length();
    uint8_t *q = query.data();
    if (qlen < kDnsHeaderSize + 5) return;

    uint8_t buf[kMaxDnsPacket];
    if (qlen + 16 > sizeof(buf)) return;

    memcpy(buf, q, qlen);
    buf[2] = 0x81;
    buf[3] = 0x80;
    buf[6] = 0x00;
    buf[7] = 0x01;
    buf[8] = 0x00;
    buf[9] = 0x00;
    buf[10] = 0x00;
    buf[11] = 0x00;

    size_t off = qlen;
    buf[off++] = 0xC0;
    buf[off++] = 0x0C;
    buf[off++] = 0x00;
    buf[off++] = 0x01;
    buf[off++] = 0x00;
    buf[off++] = 0x01;
    buf[off++] = 0x00;
    buf[off++] = 0x00;
    buf[off++] = 0x00;
    buf[off++] = 0x3C;
    buf[off++] = 0x00;
    buf[off++] = 0x04;
    buf[off++] = _apIp[0];
    buf[off++] = _apIp[1];
    buf[off++] = _apIp[2];
    buf[off++] = _apIp[3];

    _server.writeTo(buf, off, query.remoteIP(), query.remotePort());
}

void BridgeDnsProxy::forwardUpstream(AsyncUDPPacket &query) {
    size_t qlen = query.length();
    if (qlen < kDnsHeaderSize || qlen > kMaxDnsPacket) return;

    uint8_t *q = query.data();
    uint16_t origTxid = ((uint16_t)q[0] << 8) | q[1];
    uint16_t newTxid = allocTxid();

    _inflight[newTxid] = InFlight{query.remoteIP(), query.remotePort(), origTxid, millis()};

    uint8_t buf[kMaxDnsPacket];
    memcpy(buf, q, qlen);
    buf[0] = (newTxid >> 8) & 0xFF;
    buf[1] = newTxid & 0xFF;

    _upstream.write(buf, qlen);
}

void BridgeDnsProxy::onUpstreamReply(AsyncUDPPacket &reply) {
    size_t rlen = reply.length();
    if (rlen < kDnsHeaderSize || rlen > kMaxDnsReply) return;

    uint8_t *r = reply.data();
    uint16_t newTxid = ((uint16_t)r[0] << 8) | r[1];

    auto it = _inflight.find(newTxid);
    if (it == _inflight.end()) return;
    InFlight inf = it->second;
    _inflight.erase(it);

    uint8_t buf[kMaxDnsReply];
    memcpy(buf, r, rlen);
    buf[0] = (inf.origTxid >> 8) & 0xFF;
    buf[1] = inf.origTxid & 0xFF;

    _server.writeTo(buf, rlen, inf.origClient, inf.origPort);
}

void BridgeDnsProxy::sweep(uint32_t now_ms) {
    if (now_ms - _lastSweep < kSweepIntervalMs) return;
    _lastSweep = now_ms;
    for (auto it = _inflight.begin(); it != _inflight.end();) {
        if (now_ms - it->second.sentAt > kInflightTimeoutMs) it = _inflight.erase(it);
        else ++it;
    }
}
