#include "LivingCapacityPreparation.h"
#include <cassert>
using namespace LivingActivity;
int main() {
    CapacitySaleFacts f{7,100,7074,3,6,100,true,true,false,false};
    uint32_t price=0;assert(QuoteCapacitySale(f,price) && price==18);
    const auto original=f;
    f.legacyProtected=true;assert(!QuoteCapacitySale(f,price));f=original;
    f.disposable=false;assert(!QuoteCapacitySale(f,price));f=original;
    f.ownedBag=false;assert(!QuoteCapacitySale(f,price));f=original;
    f.charged=true;assert(!QuoteCapacitySale(f,price));f=original;
    f.quantity=UINT32_MAX;assert(!QuoteCapacitySale(f,price));f=original;
    f.money=INT32_MAX;assert(!QuoteCapacitySale(f,price));f=original;
    ResourceClaim c;c.id="00000000-0000-4000-8000-000000000001";
    c.task="00000000-0000-4000-8000-000000000002";c.actor=7;c.itemGuid=100;c.itemEntry=7074;
    c.quantity=3;c.state="held";c.location="bags";
    assert(ExactCapacityClaim(c,f,c.task));
    c.quantity=2;assert(!ExactCapacityClaim(c,f,c.task));c.quantity=3;
    c.location="bank";assert(!ExactCapacityClaim(c,f,c.task));c.location="bags";
    c.state="consumed";assert(!ExactCapacityClaim(c,f,c.task));c.state="held";
    assert(!ExactCapacityClaim(c,f,c.id));
    assert(VerifyCapacitySale(f,118,8,5,true,false,100,7074,3));
    assert(!VerifyCapacitySale(f,100,8,5,true,false,100,7074,3));
    assert(!VerifyCapacitySale(f,118,8,8,true,false,100,7074,3));
    assert(!VerifyCapacitySale(f,118,8,5,true,true,100,7074,3));
    assert(!VerifyCapacitySale(f,118,8,5,true,false,101,7074,3));
    assert(!VerifyCapacitySale(f,118,8,5,false,false,100,7074,3));
    assert(!VerifyCapacitySale(f,118,8,5,true,false,100,7074,2));
}
