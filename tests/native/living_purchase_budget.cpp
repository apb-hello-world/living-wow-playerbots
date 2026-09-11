#include "LivingPurchaseBudget.h"
#include <cassert>
#include <limits>
#include <thread>
using namespace LivingActivity;
int main() {
    std::string why; PurchaseLimits limits; PurchaseSpend spend{true,2,100,50};
    assert(WithinPurchaseBudget(spend,limits,900,900,100,true,why));
    assert(!WithinPurchaseBudget(spend,limits,900,900,101,true,why) && why=="purchase_daily_limit");
    spend.auctionCountHour=3;
    assert(!WithinPurchaseBudget(spend,limits,900,900,100,true,why) && why=="purchase_hourly_limit");
    assert(WithinPurchaseBudget(spend,limits,900,900,100,false,why));
    assert(!WithinPurchaseBudget(spend,limits,900,99,100,false,why));
    spend.committed=200; // Reservations never enlarge the wallet-based allowance.
    assert(!WithinPurchaseBudget(spend,limits,900,900,1,false,why));
    spend={false,0,0,0}; assert(!WithinPurchaseBudget(spend,limits,900,900,1,false,why));
    spend={true,0,std::numeric_limits<uint64_t>::max(),0};
    assert(!WithinPurchaseBudget(spend,limits,900,900,1,false,why) && why=="purchase_budget_overflow");
    assert(DecodePurchaseSpend({"2","100","50","0"},spend) && spend.complete && spend.committed==50);
    for (const auto& value : {"", "-1", "1.5", "18446744073709551616", "100x"})
        assert(!DecodePurchaseSpend({"2",value,"50","0"},spend) && !spend.complete);
    assert(!DecodePurchaseSpend({"2","0","0","1"},spend));
    const auto query=PurchaseSpendQuery(497,172800000,"5a1f5c73-ccfc-5526-add2-ac21defc7617");
    assert(query.find("o.state IN ('intent','reconciling')")!=std::string::npos);
    assert(query.find("AND o.state='intent')")!=std::string::npos);
    assert(query.find("t.actor_guid=497")!=std::string::npos);
    bool invalid=false; try { PurchaseSpendQuery(497,172800000,"' OR 1=1"); } catch (...) {invalid=true;}
    assert(invalid);
    PurchaseEpoch epochs;
    const auto first=epochs.Read(497); assert(first && !epochs.Read(0));
    epochs.Begin(497); assert(!epochs.Read(497));
    epochs.Begin(497); epochs.End(497); assert(!epochs.Read(497));
    epochs.End(497); assert(epochs.Read(497)>first);
    const auto sameSlot=epochs.Read(497); epochs.Changed(497+4096); assert(epochs.Read(497)>sameSlot);
    std::thread worker([&]{for (int i=0;i!=10000;++i) {epochs.Begin(497); epochs.End(497);}});
    for (int i=0;i!=10000;++i) (void)epochs.Read(497);
    worker.join(); assert(epochs.Read(497)>sameSlot);
}
