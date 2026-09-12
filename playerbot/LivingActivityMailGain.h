#pragma once
#include "LivingActivityItemGain.h"

namespace LivingActivity {
// The auction's exact physical stack is known before payment. The mail ID
// does not exist yet; it must come from native execution, not a planner.
struct MailGainSpec {
    uint32_t auction=0, guid=0, entry=0, quantity=0;
    bool Empty() const {return !auction && !guid && !entry && !quantity;}
};
bool ValidMailGainSpec(const MailGainSpec& spec);
std::string MailGainSpecJson(const MailGainSpec& spec);
bool VerifyNativeMailGain(uint32_t actor,const MailGainSpec& spec,
    const NativeResourceBalance& acquired,std::string& blocker);
ClaimReceiptChange MailGainClaim(const Task& task,const std::string& operation,
    const MailGainSpec& spec,const NativeResourceBalance& acquired);
ClaimedOutcome MailedOperationWrite(const Task& task,uint64_t expected,
    const OperationResult& result,const std::string& receipt,const std::string& nativeAfter,
    const std::vector<ClaimConsumption>& consumption,const MailGainSpec& spec,
    const NativeResourceBalance& acquired);
}
