#ifndef LIVING_PARTY_SERVICE_H
#define LIVING_PARTY_SERVICE_H
#include "LivingActivityEffects.h"

namespace LivingActivity {
// Permission to execute ONE already-owned parcel, not ownership of its goods
// or permission to continue the parent profession job. Native session epochs
// are deliberately ephemeral: restoration must obtain a fresh party window.
struct PartyServiceBinding {
    std::string root, claim, session, receipt;
    uint32_t actor=0, human=0, entry=0;
    uint64_t sessionRevision=0, acceptedRevision=0;
};
struct PartyServiceWindow {
    uint32_t actor=0, human=0;
    std::string session;
    uint64_t sessionRevision=0;
    bool authorized=false;
};
inline bool PartyServiceMatches(const PartyServiceBinding& binding,const Task& task,
    const PartyServiceWindow& window) {
    return window.authorized && !binding.root.empty() && !binding.claim.empty() &&
        binding.receipt.empty() && binding.actor && binding.human && binding.entry &&
        binding.sessionRevision && binding.acceptedRevision &&
        binding.actor==window.actor && binding.human==window.human &&
        binding.session==window.session && binding.sessionRevision==window.sessionRevision &&
        task.actor==binding.actor && task.id==binding.root && task.root==binding.root &&
        task.accepted && task.mode==Mode::Active && !Terminal(task.phase) &&
        task.revision>=binding.acceptedRevision;
}
inline bool PartyServiceEffects(uint32_t effects) {
    constexpr uint32_t allowed=Mask(Effect::Movement)|Mask(Effect::TravelTarget)|
        Mask(Effect::Inventory)|Mask(Effect::Money);
    return (effects & ~allowed)==0;
}
inline bool PartyServiceOperation(const PartyServiceBinding& binding,const std::string& kind,
    const ResourceClaim& claim) {
    if(kind=="mail_collect")return claim.id==binding.claim && claim.task==binding.root &&
        claim.actor==binding.actor && claim.itemEntry==binding.entry && claim.state=="held" &&
        claim.location=="mail" && claim.itemGuid && claim.nativeReference && claim.quantity;
    // These are the existing finite capacity prerequisites of this same root.
    return kind=="capacity_vendor_sale" || kind=="bank_deposit";
}
inline bool PartyServiceReceipt(PartyServiceBinding& binding,const Task& task,
    const std::string& kind,const ResourceClaim& claim,const std::string& receipt) {
    // Completion remains valid if recall happened after the atomic native
    // effect. Only the acknowledged journal callback may supply this proof.
    if(receipt.empty() || !binding.receipt.empty() || kind!="mail_collect" ||
        task.id!=binding.root || task.actor!=binding.actor || task.revision<binding.acceptedRevision ||
        !PartyServiceOperation(binding,kind,claim))return false;
    binding.receipt=receipt;return true;
}
}
#endif
