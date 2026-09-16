#include "LivingPartyService.h"
#include <cassert>
using namespace LivingActivity;
int main() {
    Task task;task.id=task.root="11111111-1111-4111-8111-111111111111";
    task.actor=97;task.kind=Kind::Profession;task.mode=Mode::Active;
    task.phase=Phase::Preparing;task.revision=4;
    PartyServiceBinding binding;binding.root=task.root;binding.actor=97;binding.human=604;
    binding.claim="22222222-2222-4222-8222-222222222222";binding.entry=765;
    binding.session="group:7:100";binding.sessionRevision=8;binding.acceptedRevision=4;
    PartyServiceWindow window{97,604,"group:7:100",8,true};
    assert(PartyServiceMatches(binding,task,window));
    auto changed=window;changed.authorized=false;assert(!PartyServiceMatches(binding,task,changed));
    changed=window;changed.human=605;assert(!PartyServiceMatches(binding,task,changed));
    changed=window;changed.actor=98;assert(!PartyServiceMatches(binding,task,changed));
    changed=window;changed.session="group:7:101";assert(!PartyServiceMatches(binding,task,changed));
    changed=window;++changed.sessionRevision;assert(!PartyServiceMatches(binding,task,changed));
    auto other=task;other.root="other";assert(!PartyServiceMatches(binding,other,window));
    other=task;other.id="other";assert(!PartyServiceMatches(binding,other,window));
    other=task;other.accepted=false;assert(!PartyServiceMatches(binding,other,window));
    other=task;other.mode=Mode::Observe;assert(!PartyServiceMatches(binding,other,window));
    other=task;--other.revision;assert(!PartyServiceMatches(binding,other,window));
    other=task;++other.revision;assert(PartyServiceMatches(binding,other,window));
    for(auto phase:{Phase::Completed,Phase::Cancelled,Phase::Failed}) {
        other=task;other.phase=phase;assert(!PartyServiceMatches(binding,other,window));
    }
    assert(PartyServiceEffects(Mask(Effect::Inventory)|Mask(Effect::Movement)|Mask(Effect::TravelTarget)|Mask(Effect::Money)));
    for(auto effect:{Effect::Spell,Effect::Group,Effect::Guild,Effect::Equipment,Effect::Social})
        assert(!PartyServiceEffects(Mask(effect)));
    ResourceClaim claim;claim.id=binding.claim;claim.task=task.root;claim.actor=97;
    claim.itemEntry=765;claim.itemGuid=123;claim.quantity=2;claim.nativeReference=456;
    claim.location="mail";claim.state="held";
    assert(PartyServiceOperation(binding,"mail_collect",claim));
    auto wrong=claim;wrong.id="other";assert(!PartyServiceOperation(binding,"mail_collect",wrong));
    wrong=claim;wrong.task="other";assert(!PartyServiceOperation(binding,"mail_collect",wrong));
    wrong=claim;wrong.location="bags";assert(!PartyServiceOperation(binding,"mail_collect",wrong));
    for(const auto kind:{"profession_craft","vendor_purchase","auction_purchase","mail_send","guild_deposit"})
        assert(!PartyServiceOperation(binding,kind,claim));
    assert(PartyServiceOperation(binding,"capacity_vendor_sale",{}));
    assert(PartyServiceOperation(binding,"bank_deposit",{}));
    assert(!PartyServiceReceipt(binding,task,"mail_collect",wrong,"receipt"));
    assert(!PartyServiceReceipt(binding,task,"bank_deposit",claim,"receipt"));
    // Recall after native effect does not erase its acknowledged proof.
    window.authorized=false;
    assert(PartyServiceReceipt(binding,task,"mail_collect",claim,"receipt"));
    assert(!PartyServiceReceipt(binding,task,"mail_collect",claim,"duplicate"));
    window.authorized=true;assert(!PartyServiceMatches(binding,task,window));
    // Reboot/restore preserves the task and claim, NEVER transient permission.
    PartyServiceBinding restored;
    assert(!PartyServiceMatches(restored,task,window));
    assert(task.revision==4 && claim.location=="mail" && claim.quantity==2);
}
