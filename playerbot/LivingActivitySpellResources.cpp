#include "LivingActivitySpellResources.h"
#include <set>
namespace LivingActivity {
    ReagentReadiness CheckUnclaimedReagents(uint32_t actor,const std::vector<ReagentNeed>& needs,
        const std::vector<ReagentStack>& bags,const ResourceView* protection) {
        if (!actor || needs.size() > 8 || bags.size() > 256) return ReagentReadiness::InvalidNativeInput;
        if (needs.empty()) return ReagentReadiness::Ready;
        if (!protection || !protection->ready) return ReagentReadiness::ProjectionUnavailable;
        std::map<uint32_t,uint64_t> required,owned;
        for (const auto& need : needs) {
            if (!need.entry || !need.quantity) return ReagentReadiness::InvalidNativeInput;
            required[need.entry]+=need.quantity;
        }
        std::set<uint32_t> identities;
        for (const auto& stack : bags) {
            if (!stack.guid || !stack.entry || !stack.quantity || !identities.insert(stack.guid).second)
                return ReagentReadiness::InvalidNativeInput;
            if (!required.count(stack.entry)) continue;
            if (stack.legacyProtected || protection->UnreservedItem(actor,stack.guid,stack.entry,stack.quantity) != stack.quantity)
                return ReagentReadiness::ProtectedStack;
            owned[stack.entry]+=stack.quantity;
        }
        for (const auto& need : required) if (owned[need.first] < need.second) return ReagentReadiness::MissingMaterial;
        return ReagentReadiness::Ready;
    }
}
