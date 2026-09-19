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
    for(const auto kind:{"profession_craft","vendor_purchase","auction_purchase","mail_send","guild_deposit",
                        "guild_bank_deposit","guild_mail_send","guild_mail_reservation"})
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
    // A guild parcel receives the same one-collection grant, never permission
    // to deposit or forward it. Existing custody and task identity stay intact.
    auto guild=task;guild.kind=Kind::GuildDelivery;guild.priority=Priority::Delivery;
    guild.source="guild_delivery";
    auto guildBinding=binding;guildBinding.receipt.clear();
    assert(PartyServiceMatches(guildBinding,guild,window));
    assert(PartyServiceOperation(guildBinding,"mail_collect",claim));
    assert(!PartyServiceOperation(guildBinding,"guild_bank_deposit",claim));
    assert(!PartyServiceOperation(guildBinding,"guild_mail_send",claim));
    assert(!PartyServiceEffects(Mask(Effect::Guild)|Mask(Effect::Inventory)));
    changed=window;++changed.sessionRevision;
    assert(!PartyServiceMatches(guildBinding,guild,changed));
    window.authorized=false;
    assert(!PartyServiceMatches(guildBinding,guild,window));
    // Recall after an atomic collection must still acknowledge that receipt.
    assert(PartyServiceReceipt(guildBinding,guild,"mail_collect",claim,"guild-collection"));
    window.authorized=true;
    assert(!PartyServiceMatches(guildBinding,guild,window));
    assert(!PartyServiceReceipt(guildBinding,guild,"mail_collect",claim,"duplicate"));
    assert(guild.priority==Priority::Delivery && guild.root==task.root && guild.revision==task.revision);
    auto repair=task;repair.kind=Kind::PartyErrand;repair.source="party_repair";
    repair.checkpoint.data="{\"workflow\":\"party_repair_v1\"}";
    auto repairBinding=binding;repairBinding.service=PartyServiceBinding::Service::Repair;
    repairBinding.claim.clear();repairBinding.entry=0;repairBinding.receipt.clear();
    assert(PartyServiceMatches(repairBinding,repair,window));
    assert(!PartyServiceMatches(repairBinding,task,window));
    changed=window;++changed.sessionRevision;assert(!PartyServiceMatches(repairBinding,repair,changed));
    assert(PartyServiceEffects(Mask(Effect::Equipment)|Mask(Effect::Money),repairBinding.service));
    assert(!PartyServiceEffects(Mask(Effect::Equipment),PartyServiceBinding::Service::Mail));
    for(auto effect:{Effect::Spell,Effect::Group,Effect::Guild,Effect::Social})
        assert(!PartyServiceEffects(Mask(effect),repairBinding.service));
    ResourceClaim money;money.id=claim.id;money.task=repair.id;money.actor=repair.actor;
    money.state="held";money.location="money";money.copper=19;
    assert(PartyServiceOperation(repairBinding,"critical_equipment_repair",money));
    for(const auto kind:{"mail_collect","capacity_vendor_sale","bank_deposit","vendor_purchase","profession_craft"})
        assert(!PartyServiceOperation(repairBinding,kind,money));
    assert(!PartyServiceOperation(guildBinding,"critical_equipment_repair",money));
    wrong=money;++wrong.actor;assert(!PartyServiceOperation(repairBinding,"critical_equipment_repair",wrong));
    wrong=money;wrong.task="another";assert(!PartyServiceOperation(repairBinding,"critical_equipment_repair",wrong));
    wrong=money;wrong.itemGuid=1;assert(!PartyServiceOperation(repairBinding,"critical_equipment_repair",wrong));
    wrong=money;wrong.state="consumed";assert(!PartyServiceOperation(repairBinding,"critical_equipment_repair",wrong));
    assert(!PartyServiceReceipt(repairBinding,repair,"critical_equipment_repair",money,"partial-repair"));
    repair.phase=Phase::Completed;
    assert(PartyServiceReceipt(repairBinding,repair,"critical_equipment_repair",money,"all-repaired"));
    assert(!PartyServiceReceipt(repairBinding,repair,"critical_equipment_repair",money,"duplicate"));
    auto vendor=task;vendor.kind=Kind::PartyErrand;vendor.source="party_vendor";
    vendor.checkpoint.data=EncodePartyVendorJob({{{123,765,2}},0});
    auto vendorBinding=repairBinding;vendorBinding.service=PartyServiceBinding::Service::Vendor;vendorBinding.receipt.clear();
    assert(PartyServiceMatches(vendorBinding,vendor,window));
    assert(!PartyServiceMatches(vendorBinding,repair,window));
    assert(!PartyServiceMatches(repairBinding,vendor,window));
    changed=window;changed.authorized=false;assert(!PartyServiceMatches(vendorBinding,vendor,changed));
    changed=window;++changed.sessionRevision;assert(!PartyServiceMatches(vendorBinding,vendor,changed));
    auto sale=claim;sale.location="bags";sale.nativeReference=0;
    assert(PartyServiceOperation(vendorBinding,"party_vendor_sale",sale));
    for(const auto kind:{"mail_collect","bank_deposit","capacity_vendor_sale","vendor_purchase","critical_equipment_repair"})
        assert(!PartyServiceOperation(vendorBinding,kind,sale));
    assert(!PartyServiceEffects(Mask(Effect::Equipment),vendorBinding.service));
    wrong=sale;wrong.state="consumed";assert(!PartyServiceOperation(vendorBinding,"party_vendor_sale",wrong));
    wrong=sale;wrong.actor=1;assert(!PartyServiceOperation(vendorBinding,"party_vendor_sale",wrong));
    assert(!PartyServiceReceipt(vendorBinding,vendor,"party_vendor_sale",sale,"partial-sale"));
    vendor.phase=Phase::Completed;
    assert(PartyServiceReceipt(vendorBinding,vendor,"party_vendor_sale",sale,"native-final-sale"));
    assert(!PartyServiceReceipt(vendorBinding,vendor,"party_vendor_sale",sale,"duplicate-sale"));
    auto auction=task;auction.kind=Kind::PartyErrand;auction.source="party_auction";
    auction.checkpoint.data=EncodePartyAuctionJob({{{123,765,2,100,720}},0});
    auto auctionBinding=repairBinding;auctionBinding.service=PartyServiceBinding::Service::Auction;auctionBinding.receipt.clear();
    assert(PartyServiceMatches(auctionBinding,auction,window));
    assert(!PartyServiceMatches(auctionBinding,vendor,window));
    assert(!PartyServiceMatches(vendorBinding,auction,window));
    changed=window;changed.authorized=false;assert(!PartyServiceMatches(auctionBinding,auction,changed));
    changed=window;++changed.sessionRevision;assert(!PartyServiceMatches(auctionBinding,auction,changed));
    assert(PartyServiceOperation(auctionBinding,"party_auction_post",sale));
    for(const auto kind:{"auction_purchase","party_vendor_sale","mail_collect","bank_deposit","vendor_purchase"})
        assert(!PartyServiceOperation(auctionBinding,kind,sale));
    assert(!PartyServiceEffects(Mask(Effect::Spell)|Mask(Effect::Equipment),auctionBinding.service));
    wrong=sale;wrong.task="wrong";assert(!PartyServiceOperation(auctionBinding,"party_auction_post",wrong));
    wrong=sale;wrong.location="bank";assert(!PartyServiceOperation(auctionBinding,"party_auction_post",wrong));
    assert(!PartyServiceReceipt(auctionBinding,auction,"party_auction_post",sale,"partial-post"));
    auction.phase=Phase::Completed;
    assert(PartyServiceReceipt(auctionBinding,auction,"party_auction_post",sale,"native-final-post"));
    assert(!PartyServiceReceipt(auctionBinding,auction,"party_auction_post",sale,"duplicate-post"));
}
