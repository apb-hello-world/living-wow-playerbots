#ifndef LIVING_PARTY_SERVICE_H
#define LIVING_PARTY_SERVICE_H
#include "LivingActivityEffects.h"
#include "LivingPartyRepair.h"
#include "LivingPartyVendor.h"
#include "LivingPartyBank.h"
#include "LivingPartyTraining.h"

namespace LivingActivity {
// Permission for one owned parcel OR one finite equipped-item repair root,
// never permission to continue another profession/guild job. Native session epochs
// are deliberately ephemeral: restoration must obtain a fresh party window.
struct PartyServiceBinding {
    std::string root, claim, session, receipt;
    uint32_t actor=0, human=0, entry=0;
    uint64_t sessionRevision=0, acceptedRevision=0;
    enum class Service { Mail, Repair, Vendor, Training, Bank } service=Service::Mail;
};
struct PartyServiceWindow {
    uint32_t actor=0, human=0;
    std::string session;
    uint64_t sessionRevision=0;
    bool authorized=false;
};
inline bool PartyServiceMatches(const PartyServiceBinding& binding,const Task& task,
    const PartyServiceWindow& window) {
    const bool repair=binding.service==PartyServiceBinding::Service::Repair;
    const bool vendor=binding.service==PartyServiceBinding::Service::Vendor;
    const bool bank=binding.service==PartyServiceBinding::Service::Bank;
    const bool training=binding.service==PartyServiceBinding::Service::Training;
    const bool subject=training ? IsPartyTrainingTask(task) && binding.claim.empty() && !binding.entry :
        bank ? IsPartyBankTask(task) && binding.claim.empty() && !binding.entry :
        vendor ? IsPartyVendorTask(task) && binding.claim.empty() && !binding.entry :
        repair ? IsPartyRepairTask(task) && binding.claim.empty() && !binding.entry :
        !binding.claim.empty() && binding.entry;
    return window.authorized && !binding.root.empty() && subject &&
        binding.receipt.empty() && !binding.session.empty() && binding.actor && binding.human &&
        binding.sessionRevision && binding.acceptedRevision &&
        binding.actor==window.actor && binding.human==window.human &&
        binding.session==window.session && binding.sessionRevision==window.sessionRevision &&
        task.actor==binding.actor && task.id==binding.root && task.root==binding.root &&
        task.accepted && task.mode==Mode::Active && !Terminal(task.phase) &&
        task.revision>=binding.acceptedRevision;
}
inline bool PartyServiceEffects(uint32_t effects,PartyServiceBinding::Service service=PartyServiceBinding::Service::Mail) {
    if(service==PartyServiceBinding::Service::Bank)
        return (effects & ~(Mask(Effect::Movement)|Mask(Effect::TravelTarget)|Mask(Effect::Inventory)))==0;
    if(service==PartyServiceBinding::Service::Training)
        return (effects & ~(Mask(Effect::Movement)|Mask(Effect::TravelTarget)|Mask(Effect::Spell)|Mask(Effect::Social)))==0;
    const uint32_t allowed=Mask(Effect::Movement)|Mask(Effect::TravelTarget)|
        Mask(Effect::Inventory)|Mask(Effect::Money)|
        (service==PartyServiceBinding::Service::Repair?Mask(Effect::Equipment):0);
    return (effects & ~allowed)==0;
}
inline bool PartyServiceOperation(const PartyServiceBinding& binding,const std::string& kind,
    const ResourceClaim& claim) {
    if(binding.service==PartyServiceBinding::Service::Bank)
        return kind=="bank_deposit" && IsUuid(claim.id) && claim.task==binding.root && claim.actor==binding.actor &&
            claim.state=="held" && claim.location=="bags" && claim.itemGuid && claim.itemEntry && claim.quantity &&
            !claim.copper && !claim.nativeReference;
    if(binding.service==PartyServiceBinding::Service::Training)
        return kind=="party_training_learn" && claim.id.empty() && !claim.copper && !claim.itemGuid && !claim.quantity;
    if(binding.service==PartyServiceBinding::Service::Vendor)
        return kind=="party_vendor_sale" && IsUuid(claim.id) && claim.task==binding.root &&
            claim.actor==binding.actor && claim.state=="held" && claim.location=="bags" &&
            claim.itemGuid && claim.itemEntry && claim.quantity && !claim.copper && !claim.nativeReference;
    if(binding.service==PartyServiceBinding::Service::Repair)
        return kind=="critical_equipment_repair" && claim.task==binding.root &&
            claim.actor==binding.actor && claim.state=="held" && claim.location=="money" &&
            !claim.id.empty() && claim.copper && !claim.itemGuid && !claim.itemEntry &&
            !claim.quantity && !claim.nativeReference;
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
    const bool repair=binding.service==PartyServiceBinding::Service::Repair;
    const bool vendor=binding.service==PartyServiceBinding::Service::Vendor;
    const bool training=binding.service==PartyServiceBinding::Service::Training;
    if(receipt.empty() || !binding.receipt.empty() ||
        (binding.service==PartyServiceBinding::Service::Bank ? !IsPartyBankTask(task) || task.phase!=Phase::Completed :
         training ? !IsPartyTrainingTask(task) || task.phase!=Phase::Completed :
         vendor ? !IsPartyVendorTask(task) || task.phase!=Phase::Completed :
         repair ? !IsPartyRepairTask(task) || task.phase!=Phase::Completed : kind!="mail_collect") ||
        task.id!=binding.root || task.actor!=binding.actor || task.revision<binding.acceptedRevision ||
        !PartyServiceOperation(binding,kind,claim))return false;
    binding.receipt=receipt;return true;
}
}
#endif
