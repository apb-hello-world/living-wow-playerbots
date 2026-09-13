#pragma once
#include "LivingCraftCapture.h"
#include "LivingActivityClaimConsumption.h"
#include <algorithm>
#include <set>

namespace LivingActivity {
// Frame order is the pinned core's DestroyItemCount order: backpack, then
// carried bags by equipped slot. The native collector rejects relevant keyring
// inputs. Never sort by GUID, item value, or pointer before selecting inputs.
struct ProfessionInputStack { NativeItemStack item; uint32_t used=0; };
// Several purchase receipts may reserve portions of one merged stack. Unused
// portions of THIS root remain held; another root or pending hold still blocks.
inline bool ProfessionInputProtectionMatches(uint32_t nativeCount,uint64_t inputClaims,
    uint64_t heldByRoot,uint64_t protectedTotal) {
    return inputClaims && inputClaims<=heldByRoot && heldByRoot<=nativeCount && heldByRoot==protectedTotal;
}
inline bool PlanProfessionInputStacks(const ProfessionJob& job,const CraftFrame& frame,
    std::vector<ProfessionInputStack>& inputs,std::string& blocker) {
    inputs.clear();
    auto reject=[&](const char* why){blocker=why;return false;};
    if(!ValidateProfessionJob(job,blocker) || !ValidCraftFrame(frame) || job.reagents.empty())
        return reject("profession_input_snapshot_invalid");
    std::vector<ProfessionInputStack> planned;
    for(const auto& need:job.reagents) {
        uint32_t remaining=need.perAttempt;
        for(const auto& item:frame.stacks) if(item.entry==need.entry && remaining) {
            const auto used=std::min(remaining,item.count);
            planned.push_back({item,used});remaining-=used;
            if(planned.size()>16)return reject("profession_input_stack_bound");
        }
        if(remaining)return reject("profession_attempt_native_material_missing");
    }
    inputs=std::move(planned);blocker.clear();return true;
}
// Every portion is tied to the physical stack the core will actually consume.
// A later stack cannot substitute for an earlier protected or changed stack.
inline bool MatchProfessionInputClaims(const Task& task,const ProfessionJob& job,const CraftFrame& frame,
    const std::vector<ClaimConsumption>& uses,std::string& blocker) {
    std::vector<ProfessionInputStack> inputs;
    auto reject=[&](){blocker="native_craft_exact_input_claims_required";return false;};
    if(frame.actor!=task.actor || uses.empty() || uses.size()>16 ||
        !PlanProfessionInputStacks(job,frame,inputs,blocker))return reject();
    std::set<std::string> matched;
    for(const auto& input:inputs) {
        uint64_t held=0,used=0;
        for(const auto& use:uses) if(use.before.itemGuid==input.item.guid) {
            const auto& c=use.before;
            if(!ValidResourceClaim(c) || c.actor!=task.actor || c.task!=task.root || c.itemEntry!=input.item.entry ||
                c.state!="held" || c.location!="bags" || c.copper || c.nativeReference ||
                !use.used || use.used>c.quantity || !matched.insert(c.id).second ||
                c.quantity>input.item.count-held)return reject();
            held+=c.quantity;used+=use.used;
        }
        if(used!=input.used || held>input.item.count)return reject();
    }
    if(matched.size()!=uses.size())return reject();
    blocker.clear();return true;
}
}
