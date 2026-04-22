#include "traffic_tap.h"

#include <algorithm>

namespace {

std::vector<ITrafficTap *> &taps() {
    static std::vector<ITrafficTap *> instance;
    return instance;
}

}  // namespace

void TrafficTapRegistry::register_tap(ITrafficTap *tap) {
    if (tap == nullptr) return;
    auto &v = taps();
    if (std::find(v.begin(), v.end(), tap) == v.end()) v.push_back(tap);
}

void TrafficTapRegistry::unregister_tap(ITrafficTap *tap) {
    auto &v = taps();
    v.erase(std::remove(v.begin(), v.end(), tap), v.end());
}

void TrafficTapRegistry::dispatch_inspect(pbuf *p, netif *iface, TapDirection dir) {
    for (auto *t : taps()) t->inspect(p, iface, dir);
}

void TrafficTapRegistry::dispatch_mutate(pbuf *&p, netif *iface, TapDirection dir) {
    for (auto *t : taps()) t->mutate(p, iface, dir);
}

size_t TrafficTapRegistry::size() { return taps().size(); }
