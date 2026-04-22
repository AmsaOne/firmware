#ifndef __TRAFFIC_TAP_H__
#define __TRAFFIC_TAP_H__

#include <cstddef>
#include <cstdint>
#include <vector>

struct pbuf;
struct netif;

enum class TapDirection : uint8_t {
    Ingress,
    Egress,
};

class ITrafficTap {
public:
    virtual ~ITrafficTap() = default;
    virtual void inspect(pbuf * /*p*/, netif * /*iface*/, TapDirection /*dir*/) {}
    virtual void mutate(pbuf *& /*p*/, netif * /*iface*/, TapDirection /*dir*/) {}
};

namespace TrafficTapRegistry {

void register_tap(ITrafficTap *tap);
void unregister_tap(ITrafficTap *tap);
void dispatch_inspect(pbuf *p, netif *iface, TapDirection dir);
void dispatch_mutate(pbuf *&p, netif *iface, TapDirection dir);
size_t size();

}  // namespace TrafficTapRegistry

#endif
