#ifndef __BRIDGE_DNS_PROXY_H__
#define __BRIDGE_DNS_PROXY_H__

#include <AsyncUDP.h>
#include <IPAddress.h>

#include <cstdint>
#include <map>
#include <set>

class BridgeDnsProxy {
public:
    explicit BridgeDnsProxy(std::set<uint32_t> &authedIps);
    ~BridgeDnsProxy();

    bool start(IPAddress apIp);
    void stop();
    bool isRunning() const { return _running; }

    void setUpstream(IPAddress dns) { _upstreamDns = dns; }
    IPAddress upstream() const { return _upstreamDns; }

    void sweep(uint32_t now_ms);

private:
    struct InFlight {
        IPAddress origClient;
        uint16_t origPort;
        uint16_t origTxid;
        uint32_t sentAt;
    };

    void onIncomingQuery(AsyncUDPPacket &pkt);
    void onUpstreamReply(AsyncUDPPacket &reply);
    void sendHijackReply(AsyncUDPPacket &query);
    void forwardUpstream(AsyncUDPPacket &query);
    uint16_t allocTxid();

    std::set<uint32_t> &_authedIps;
    AsyncUDP _server;
    AsyncUDP _upstream;
    std::map<uint16_t, InFlight> _inflight;
    IPAddress _apIp;
    IPAddress _upstreamDns{8, 8, 8, 8};
    bool _running = false;
    uint32_t _lastSweep = 0;

    static constexpr uint32_t kInflightTimeoutMs = 5000;
    static constexpr uint32_t kSweepIntervalMs = 2000;
};

#endif
