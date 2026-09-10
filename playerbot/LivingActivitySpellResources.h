#ifndef LIVING_ACTIVITY_SPELL_RESOURCES_H
#define LIVING_ACTIVITY_SPELL_RESOURCES_H
#include "LivingActivityResourceView.h"
#include <cstdint>
#include <map>
#include <vector>

namespace LivingActivity {
    enum class ReagentReadiness { Ready, ProjectionUnavailable, ProtectedStack, MissingMaterial, InvalidNativeInput };
    struct ReagentNeed { uint32_t entry=0,quantity=0; };
    struct ReagentStack { uint32_t guid=0,entry=0,quantity=0; bool legacyProtected=false; };
    // Native spells choose their own stacks. Summing unreserved portions is
    // insufficient: the native operation might take a protected portion first.
    // Until a service adapter prepares a safe split, any protected matching
    // stack defers the ordinary cast. This function never changes possessions.
    ReagentReadiness CheckUnclaimedReagents(uint32_t actor,const std::vector<ReagentNeed>& needs,
        const std::vector<ReagentStack>& bags,const ResourceView* protection);
}
#endif
