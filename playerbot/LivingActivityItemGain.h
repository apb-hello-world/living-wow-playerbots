#ifndef LIVING_ACTIVITY_ITEM_GAIN_H
#define LIVING_ACTIVITY_ITEM_GAIN_H
#include "LivingActivityClaimConsumption.h"

namespace LivingActivity {
    // One exact purchased output. Variable/random crafting outputs and native
    // transfers require their own validators; they cannot claim this proof.
    struct ItemGainSpec {
        uint32_t entry = 0, quantity = 0;
        bool Empty() const { return !entry && !quantity; }
    };
    constexpr size_t MaximumItemGainStacks = 15; // Plus one consumed money claim.
    struct NativeItemStack {
        uint32_t actor = 0, guid = 0, entry = 0, count = 0, bagGuid = 0;
        uint8_t slot = 0;
    };
    struct VerifiedItemGain { NativeItemStack before, after; uint32_t added = 0; };
    bool ValidItemGainSpec(const ItemGainSpec& spec);
    std::string ItemGainSpecJson(const ItemGainSpec& spec);
    // Actual native stack identities/locations, never totals alone. Unchanged
    // existing stacks must remain; only the requested positive gain is allowed.
    bool VerifyNativeItemGain(uint32_t actor, const ItemGainSpec& spec,
        const std::vector<NativeItemStack>& before, const std::vector<NativeItemStack>& after,
        std::vector<VerifiedItemGain>& gains, std::string& blocker);
    std::string ItemGainClaimId(const std::string& operation, uint32_t itemGuid);
    std::vector<ClaimReceiptChange> ItemGainClaims(const Task& task, const std::string& operation,
        const ItemGainSpec& spec, const std::vector<VerifiedItemGain>& gains);
    // Native output claims and consumed input claims commit with the SAME
    // operation receipt. This function alone does not perform/verify gameplay.
    ClaimedOutcome AcquiredOperationWrite(const Task& task, uint64_t expected,
        const OperationResult& result, const std::string& receipt, const std::string& nativeAfter,
        const std::vector<ClaimConsumption>& consumption, const ItemGainSpec& spec,
        const std::vector<VerifiedItemGain>& gains);
}
#endif
