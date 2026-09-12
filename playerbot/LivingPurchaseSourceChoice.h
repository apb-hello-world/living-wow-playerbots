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
};
enum class PurchaseSourcePreference { Vendor, Auction, Wait };
inline double PurchaseSourceScore(const PurchaseSourceCandidate& value) {
    if(!value.available || !value.copper || !value.quantity ||
        !std::isfinite(value.distance) || value.distance<0)
        return std::numeric_limits<double>::infinity();
    return double(value.copper)/value.quantity*(1.0+std::min(value.distance/500.0,4.0));
}
inline bool PreferAuctionSource(const PurchaseSourceCandidate& vendor,const PurchaseSourceCandidate& auction) {
    const auto a=PurchaseSourceScore(auction),v=PurchaseSourceScore(vendor);
    // Stable vendor tie-break and a small margin avoid changing equal choices.
    return std::isfinite(a) && a<v*0.95;
}
}
