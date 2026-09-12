#pragma once
#include "LivingActivityResources.h"
#include <limits>

namespace LivingActivity {
// A capacity sale is one complete, explicitly reserved bag stack. It never
// takes a partly reserved stack or treats disappearing need flags as proof.
struct CapacitySaleFacts {
    uint32_t actor=0,guid=0,entry=0,quantity=0,unitCopper=0,money=0;
    bool ownedBag=false,disposable=false,legacyProtected=false,charged=false;
};
inline bool QuoteCapacitySale(const CapacitySaleFacts& f,uint32_t& copper) {
    copper=0;
    const uint64_t price=uint64_t(f.unitCopper)*f.quantity;
    if (!f.actor || !f.guid || !f.entry || !f.quantity || !price || !f.ownedBag ||
        !f.disposable || f.legacyProtected || f.charged ||
        price+f.money>uint64_t(std::numeric_limits<int32_t>::max())) return false;
    copper=uint32_t(price);return true;
}
inline bool ExactCapacityClaim(const ResourceClaim& c,const CapacitySaleFacts& f,const std::string& root) {
    return ValidResourceClaim(c) && c.task==root && c.actor==f.actor && c.state=="held" &&
        c.location=="bags" && !c.copper && !c.nativeReference && c.itemGuid==f.guid &&
        c.itemEntry==f.entry && c.quantity==f.quantity;
}
inline bool VerifyCapacitySale(const CapacitySaleFacts& before,uint32_t moneyAfter,
    uint32_t entryCountBefore,uint32_t entryCountAfter,bool oldSlotEmpty,bool originalInBags,
    uint32_t buybackGuid,uint32_t buybackEntry,uint32_t buybackCount) {
    uint32_t price=0;
    return QuoteCapacitySale(before,price) && moneyAfter==uint64_t(before.money)+price &&
        entryCountBefore>=before.quantity && entryCountAfter==entryCountBefore-before.quantity &&
        oldSlotEmpty && !originalInBags && buybackGuid==before.guid &&
        buybackEntry==before.entry && buybackCount==before.quantity;
}
}
