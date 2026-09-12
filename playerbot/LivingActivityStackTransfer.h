#pragma once
#include "LivingActivityItemGain.h"
#include <limits>

namespace LivingActivity {
// A single native whole-stack relocation. The destination is either empty or
// one existing stack. Multi-destination splits have a different proof contract.
// This is evidence from one atomic native call, never an inference from totals.
inline bool VerifyWholeStackTransfer(const NativeItemStack& source,
    const NativeItemStack& destinationBefore,const NativeItemStack& destinationAfter,
    bool sourceStillPresent,std::string& blocker) {
    auto reject=[&](const char* why){blocker=why;return false;};
    if (!source.actor || !source.guid || !source.entry || !source.count ||
        destinationAfter.actor!=source.actor || destinationAfter.entry!=source.entry ||
        !destinationAfter.guid || !destinationAfter.count)
        return reject("native_transfer_identity_invalid");
    if (!destinationBefore.guid) {
        if (destinationBefore.actor || destinationBefore.entry || destinationBefore.count ||
            destinationBefore.bagGuid || destinationBefore.slot ||
            destinationAfter.guid!=source.guid || destinationAfter.count!=source.count)
            return reject("native_transfer_empty_destination_mismatch");
    } else {
        if (destinationBefore.actor!=source.actor || destinationBefore.entry!=source.entry ||
            destinationBefore.guid==source.guid || !destinationBefore.count ||
            destinationAfter.guid!=destinationBefore.guid ||
            destinationAfter.bagGuid!=destinationBefore.bagGuid || destinationAfter.slot!=destinationBefore.slot ||
            uint64_t(destinationBefore.count)+source.count>std::numeric_limits<uint32_t>::max() ||
            uint64_t(destinationAfter.count)!=uint64_t(destinationBefore.count)+source.count || sourceStillPresent)
            return reject("native_transfer_merge_identity_mismatch");
    }
    blocker.clear();return true;
}
}
