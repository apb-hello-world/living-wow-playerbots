#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace LivingActivity {
// Read-only source ranking, not permission to purchase or a route guarantee.
// Travel adds at most four times unit price to the ranking. This permits a
// modest local premium without inventing spendable money or changing caps.
struct PurchaseSourceCandidate {
    uint32_t copper=0, quantity=0;
    double distance=0;
    bool available=false;
    bool mapTransfer=false;
};
enum class PurchaseSourcePreference { Vendor, Auction, Wait };
inline double PurchaseTravelDistance(double distance,bool mapTransfer) {
    if(!std::isfinite(distance) || distance<0)return std::numeric_limits<double>::infinity();
    // The native inter-map geometric shortcut does not price waiting/riding a
    // transport. Use the existing maximum travel premium for that uncertainty;
    // this is a ranking weight, NOT measured yards or an arrival-time promise.
    return mapTransfer ? std::max(distance,2000.0) : distance;
}
inline double PurchaseSourceScore(const PurchaseSourceCandidate& value) {
    if(!value.available || !value.copper || !value.quantity ||
        !std::isfinite(value.distance) || value.distance<0)
        return std::numeric_limits<double>::infinity();
    return double(value.copper)/value.quantity*(1.0+std::min(PurchaseTravelDistance(value.distance,value.mapTransfer)/500.0,4.0));
}
inline bool PreferAuctionSource(const PurchaseSourceCandidate& vendor,const PurchaseSourceCandidate& auction) {
    const auto a=PurchaseSourceScore(auction),v=PurchaseSourceScore(vendor);
    // Stable vendor tie-break and a small margin avoid changing equal choices.
    return std::isfinite(a) && a<v*0.95;
}
}
