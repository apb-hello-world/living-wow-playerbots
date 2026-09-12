#include "LivingVendorSources.h"
#include "LivingServiceTravel.h"
#include <cassert>
using namespace LivingActivity;
int main() {
    VendorSourceIndex sources(3);
    sources.Add(3371,3348);sources.Add(3371,3348);sources.Add(3371,1313);
    assert(!sources.Ready() && sources.Sellers(3371).empty());
    sources.Add(0,1);sources.Add(1,0);sources.Seal();
    assert(sources.Ready() && sources.Sellers(3371)==std::set<uint32_t>({1313,3348}));
    // A completed catalogue is immutable during bot execution.
    sources.Add(3371,999);assert(sources.Sellers(3371).size()==2);
    assert(sources.Sellers(999).empty());
    sources.Clear();assert(!sources.Ready() && sources.Sellers(3371).empty());
    sources.Add(3371,1);sources.Add(3371,2);sources.Add(3371,3);sources.Add(3371,4);sources.Seal();
    assert(!sources.Ready() && sources.Sellers(3371).empty());
    sources.Clear();sources.Add(3371,1);sources.Seal();assert(sources.Ready());
    ServiceDestination parsed;
    assert(ParseServiceStep("profession_service_purchase_vendor",parsed) && parsed==ServiceDestination::PurchaseVendor);
    assert(ParseServiceStep("profession_service_capacity_vendor",parsed) && parsed==ServiceDestination::Vendor);
    assert(!ParseServiceStep("profession_vendor_purchase",parsed)); // Execution is not travel.
}
