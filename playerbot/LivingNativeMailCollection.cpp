#include "botpch.h"
#include "LivingNativeMailCollection.h"
#include "LivingMailRecovery.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityNativeContext.h"
#include "LivingActivityTransfer.h"
#include "LivingActivityStackTransfer.h"
#include "LivingServiceExecution.h"
#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"
#include "playerbot/strategy/values/ItemUsageValue.h"
#include "Mails/Mail.h"
#include "Database/DatabaseImpl.h"
#include <boost/property_tree/json_parser.hpp>
#include <sstream>
namespace LivingActivity {
namespace {
bool SafeMailActor(Player& actor) {
    return actor.GetPlayerbotAI() && actor.GetSession() && actor.IsInWorld() && actor.IsAlive() &&
        !actor.IsBeingTeleported() && !ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) &&
        actor.IsStopped() && !actor.GetMap()->IsDungeon() && !LivingServiceExecution::Busy(&actor);
}
bool NativeDestination(Player& actor,Item& item,uint16_t& destination,uint32_t& mergeGuid,uint32_t& mergeCount) {
    mergeGuid=mergeCount=0;
    // Use the handler's exact automatic placement. One whole-stack merge is
    // journalled below; a multi-destination split is not this adapter's proof.
    ItemPosCountVec positions;uint8_t affected=0;
    if (actor.CanStoreItem(NULL_BAG,NULL_SLOT,positions,&item,affected,false)!=EQUIP_ERR_OK ||
        positions.size()!=1 || positions[0].count!=item.GetCount()) return false;
    destination=positions[0].pos;
    if (!Player::IsInventoryPos(uint8_t(destination>>8),uint8_t(destination))) return false;
    if (const auto* target=actor.GetItemByPos(destination)) {
        const auto view=sLivingActivityCoordinator.ResourceReservations().Inspect();
        if (!view || !view->ready || target->GetEntry()!=item.GetEntry() || target->GetGUIDLow()==item.GetGUIDLow() ||
            view->HasUncertainItem(actor.GetGUIDLow(),item.GetEntry()) ||
            view->ProtectedItem(target->GetGUIDLow())>target->GetCount()) return false;
        mergeGuid=target->GetGUIDLow();mergeCount=target->GetCount();
    }
    return true;
}
NativeItemStack Stack(Player& actor,const Item* item) {
    if (!item) return {};
    return {actor.GetGUIDLow(),item->GetGUIDLow(),item->GetEntry(),item->GetCount(),
        item->GetContainer()?item->GetContainer()->GetGUIDLow():0,item->GetSlot()};
}
bool ProtectedLegacy(Player& actor,const ResourceClaim& c) {
    return sPlayerbotActionBroker.ReservedItemsView()->Item(c.itemGuid) ||
        ((sGuildSupplies.Reserved(c.itemGuid) || sGuildSupplies.ReservedEntry(c.actor,c.itemEntry)) &&
         !sGuildSupplies.AllowsManagedClaim(c)) ||
        ai::ItemUsageValue::IsNeededForQuest(&actor,c.itemEntry,true);
}
}
bool ReadNativeMailBalance(Player& actor,const ResourceClaim& c,NativeResourceBalance& balance) {
    balance={};
    if (!sLivingActivityCoordinator.OnWorldThread() || c.actor!=actor.GetGUIDLow() || !ValidMailTransfer(c)) return false;
    const auto* mail=actor.GetMail(uint32_t(c.nativeReference));
    const auto* item=actor.GetMItem(c.itemGuid);
    if (!mail || mail->receiverGuid!=actor.GetObjectGuid() || mail->state==MAIL_STATE_DELETED ||
        mail->expire_time<=time(nullptr) || !item || item->GetOwnerGuid()!=actor.GetObjectGuid() ||
        item->GetEntry()!=c.itemEntry || item->GetCount()!=c.quantity) return false;
    const auto matches=std::count_if(mail->items.begin(),mail->items.end(),[&](const MailItemInfo& a) {
        return a.item_guid==c.itemGuid && a.item_template==c.itemEntry;
    });
    if (matches!=1) return false;
    balance={c.actor,c.itemGuid,c.itemEntry,item->GetCount(),0,"mail",c.nativeReference};return true;
}
uint64_t NativeNearbyMailbox(Player& actor) {
    if (!SafeMailActor(actor)) return 0;
    for (const auto guid : actor.GetPlayerbotAI()->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("nearest game objects no los")->Get())
        if (actor.GetGameObjectIfCanInteractWith(guid,GAMEOBJECT_TYPE_MAILBOX)) return guid.GetRawValue();
    return 0;
}
bool PlanNativeMailCollection(Player& actor,const Task& task,const ResourceClaim& c,NativeMailQuote& q,std::string& blocker) {
    q={};auto reject=[&](const char* why){blocker=why;return false;};
    if (!sLivingActivityCoordinator.OnWorldThread() || task.actor!=actor.GetGUIDLow() || !SafeMailActor(actor))
        return reject("profession_mail_safety_pause");
    NativeResourceBalance balance;
    if (c.task!=task.root || !ReadNativeMailBalance(actor,c,balance)) return reject("profession_mail_attachment_requires_reconciliation");
    if(IsManagedGuildDelivery(task)) {
        ResourceClaim exact;
        if(!sGuildSupplies.ReadManagedMail(task,exact,blocker))return false;
        exact.id=c.id;exact.revision=c.revision;
        if(!SameResourceClaim(c,exact))return reject("guild_delivery_mail_claim_changed");
    }
    const auto* mail=actor.GetMail(uint32_t(c.nativeReference));
    if (mail->COD) return reject("profession_cod_material_requires_explicit_acceptance");
    if (mail->deliver_time>time(nullptr)) return reject("profession_mail_delivery_pending");
    if (actor.m_mailsUpdated || mail->state!=MAIL_STATE_UNCHANGED || !mail->removedItems.empty())
        return reject("profession_mail_native_save_pending");
    if (ProtectedLegacy(actor,c)) return reject("profession_mail_has_legacy_commitment");
    uint32_t available=0;
    if (!sLivingActivityCoordinator.TaskResourceAvailability(task.id,task.revision,balance,available,blocker)) return false;
    if (available!=c.quantity) return reject("profession_mail_stack_has_other_commitments");
    const auto mailbox=NativeNearbyMailbox(actor);
    if (!mailbox) return reject("profession_mailbox_travel_required");
    auto* item=actor.GetMItem(c.itemGuid);uint16_t to=0;
    if (!NativeDestination(actor,*item,to,q.mergeGuid,q.mergeCount)) return reject("profession_mail_single_destination_required");
    q.actor=c.actor;q.mail=uint32_t(c.nativeReference);q.guid=c.itemGuid;q.entry=c.itemEntry;q.quantity=c.quantity;
    q.mailbox=mailbox;q.mailboxEntry=actor.GetGameObjectIfCanInteractWith(ObjectGuid(mailbox),GAMEOBJECT_TYPE_MAILBOX)->GetEntry();
    q.deliveredAt=mail->deliver_time;q.expiresAt=mail->expire_time;q.moneyBefore=actor.GetMoney();q.mailMoney=mail->money;
    q.attachmentsBefore=uint32_t(mail->items.size());q.bagBefore=actor.GetItemCount(c.itemEntry,false);
    q.totalBefore=actor.GetItemCount(c.itemEntry,true);q.to=to;blocker.clear();return true;
}
bool ReadUncollectedNativeMail(Player& actor,const NativeMailQuote& before,MailRecoverySnapshot& result,std::string& why) {
    result={};auto reject=[&](const char* s){why=s;return false;};
    if(!sLivingActivityCoordinator.OnWorldThread() || actor.GetGUIDLow()!=before.actor || !SafeMailActor(actor))
        return reject("interrupted_mail_safety_pause");
    const auto* mail=actor.GetMail(before.mail);const auto* item=actor.GetMItem(before.guid);
    if(!mail || !item || mail->receiverGuid!=actor.GetObjectGuid() || mail->state!=MAIL_STATE_UNCHANGED ||
        mail->COD || mail->expire_time<=time(nullptr) || actor.m_mailsUpdated || !mail->removedItems.empty() ||
        item->GetOwnerGuid()!=actor.GetObjectGuid() ||
        std::count_if(mail->items.begin(),mail->items.end(),[&](const MailItemInfo& a){
            return a.item_guid==before.guid && a.item_template==before.entry;
        })!=1 || actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,before.guid)))
        return reject("interrupted_mail_attachment_changed");
    result.unchanged=before;auto& q=result.unchanged;
    q.entry=item->GetEntry();q.quantity=item->GetCount();q.deliveredAt=mail->deliver_time;q.expiresAt=mail->expire_time;
    q.mailMoney=mail->money;q.attachmentsBefore=mail->items.size();q.moneyBefore=actor.GetMoney();
    q.bagBefore=actor.GetItemCount(q.entry,false);q.totalBefore=actor.GetItemCount(q.entry,true);
    result.destination=Stack(actor,actor.GetItemByPos(before.to));
    q.mergeGuid=result.destination.guid;q.mergeCount=result.destination.count;
    if(!q.mergeGuid && uint8_t(before.to>>8)!=INVENTORY_SLOT_BAG_0) {
        const auto* bag=actor.GetItemByPos(INVENTORY_SLOT_BAG_0,uint8_t(before.to>>8));
        if(!bag || !bag->IsBag())return reject("interrupted_mail_destination_changed");
        result.destination.bagGuid=bag->GetGUIDLow();
    }
    why.clear();return true;
}
bool NativeMailCollection::ValidateNative(Player& actor,const OperationRequest& r,std::string& blocker) {
    NativeMailQuote current;
    // Re-read the exact native envelope/attachment, placement and permissions
    // before dispatch. A task receipt is not evidence that mail has arrived.
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(r.transition.task.id);
    if (!saved || saved->actor!=actor.GetGUIDLow() || saved->checkpoint.data!=r.transition.task.checkpoint.data ||
        (saved->revision!=r.transition.expectedRevision && saved->revision!=r.transition.task.revision)) {
        blocker="profession_mail_saved_intent_changed";return false;
    }
    if (!PlanNativeMailCollection(actor,*saved,r.itemTransfer,current,blocker)) return false;
    if (EncodeNativeMailQuote(current)!=EncodeNativeMailQuote(quote) || r.beforeState!=EncodeNativeMailQuote(quote)) {
        blocker="profession_mail_quote_changed";return false;
    }
    blocker.clear();return true;
}
NativeObservation NativeMailCollection::ExecuteNative(Player& actor,const OperationRequest& request) {
    NativeObservation out;std::string blocker;
    if (!ValidateNative(actor,request,blocker)) {out.state=OperationState::Rejected;out.evidence=blocker;return out;}
    if (!CharacterDatabase.HasOpenTransaction()) {out.state=OperationState::Rejected;out.evidence="mail_atomic_transaction_required";return out;}
    const NativeItemStack source{quote.actor,quote.guid,quote.entry,quote.quantity,0,0};
    const auto destinationBefore=Stack(actor,actor.GetItemByPos(quote.to));
    WorldPacket packet(CMSG_MAIL_TAKE_ITEM,16);packet<<ObjectGuid(quote.mailbox)<<uint32(quote.mail)<<uint32(quote.guid);
    actor.GetSession()->HandleMailTakeItem(packet);
    const auto survivingGuid=quote.mergeGuid ? quote.mergeGuid : quote.guid;
    auto* item=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,survivingGuid));const auto* mail=actor.GetMail(quote.mail);
    const auto destinationAfter=Stack(actor,item);
    const bool identityVerified=VerifyWholeStackTransfer(source,destinationBefore,destinationAfter,
        actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,quote.guid))!=nullptr,blocker);
    const bool removed=mail && std::none_of(mail->items.begin(),mail->items.end(),[&](const MailItemInfo& a){return a.item_guid==quote.guid;});
    out.nativeReference="mail:"+std::to_string(quote.mail)+":item:"+std::to_string(quote.guid);
    out.afterState="{\"mail\":"+std::to_string(quote.mail)+",\"guid\":"+std::to_string(quote.guid)+",\"to\":"+std::to_string(quote.to)+
        ",\"bag\":"+std::to_string(item && item->GetContainer() ? item->GetContainer()->GetGUIDLow() : 0)+
        ",\"slot\":"+std::to_string(item ? item->GetSlot() : 0)+",\"bag_count\":"+std::to_string(actor.GetItemCount(quote.entry,false))+
        ",\"total_count\":"+std::to_string(actor.GetItemCount(quote.entry,true))+",\"money\":"+std::to_string(actor.GetMoney())+
        ",\"surviving_guid\":"+std::to_string(destinationAfter.guid)+",\"surviving_count\":"+std::to_string(destinationAfter.count)+'}';
    if (CharacterDatabase.HasOpenTransaction() && item && item->GetOwnerGuid()==actor.GetObjectGuid() && item->GetPos()==quote.to &&
        identityVerified && !actor.GetMItem(quote.guid) && removed &&
        mail->items.size()+1==quote.attachmentsBefore && !mail->COD && mail->money==quote.mailMoney &&
        actor.GetMoney()==quote.moneyBefore && uint64_t(actor.GetItemCount(quote.entry,false))==uint64_t(quote.bagBefore)+quote.quantity &&
        uint64_t(actor.GetItemCount(quote.entry,true))==uint64_t(quote.totalBefore)+quote.quantity) {
        out.state=OperationState::Verified;out.evidence="native_mail_attachment_collected";
        out.transferredItem={quote.actor,survivingGuid,quote.entry,item->GetCount(),0,"bags"};
    } else out.evidence="native_mail_collection_requires_reconciliation";
    auto* context=actor.GetPlayerbotAI()->GetAiObjectContext();
    for (const auto& name : context->GetValues()) {
        const auto base=name.substr(0,name.find("::"));
        if (base=="bag space" || base=="item usage" || base=="item count" || base=="inventory items" || base=="inventory item ids")
            if (auto* value=context->GetUntypedValue(name)) value->Reset();
    }
    return out;
}
std::string NativeMailCollection::PersistedNativeProof(Player& actor,const OperationRequest& request,const Task& outcome) const {
    const auto survivingGuid=quote.mergeGuid ? quote.mergeGuid : quote.guid;
    const auto* item=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,survivingGuid));
    if (!item) return {};
    const auto bag=item->GetContainer() ? item->GetContainer()->GetGUIDLow() : 0;
    std::string proof="SELECT "+SqlValue(outcome.id)+','+std::to_string(outcome.revision)+
        " FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid="+std::to_string(quote.actor)+
        " AND v.item="+std::to_string(survivingGuid)+" AND v.item_template="+std::to_string(quote.entry)+
        " AND v.bag="+std::to_string(bag)+" AND v.slot="+std::to_string(item->GetSlot())+
        " AND i.owner_guid="+std::to_string(quote.actor)+" AND i.itemEntry="+std::to_string(quote.entry)+" AND i.count="+std::to_string(uint64_t(quote.quantity)+quote.mergeCount)+
        (quote.mergeGuid ? " AND NOT EXISTS(SELECT 1 FROM item_instance removed WHERE removed.guid="+std::to_string(quote.guid)+')' : "")+
        " AND NOT EXISTS(SELECT 1 FROM mail_items mi WHERE mi.item_guid="+std::to_string(quote.guid)+')'+
        " AND EXISTS(SELECT 1 FROM mail m WHERE m.id="+std::to_string(quote.mail)+" AND m.receiver="+std::to_string(quote.actor)+
        " AND m.cod=0 AND m.money="+std::to_string(quote.mailMoney)+" AND m.deliver_time="+std::to_string(quote.deliveredAt)+
        " AND m.expire_time="+std::to_string(quote.expiresAt)+')'+
        " AND (SELECT COUNT(*) FROM mail_items mi WHERE mi.mail_id="+std::to_string(quote.mail)+")="+std::to_string(quote.attachmentsBefore-1)+
        " AND EXISTS(SELECT 1 FROM characters c WHERE c.guid="+std::to_string(quote.actor)+" AND c.money="+std::to_string(quote.moneyBefore)+')';
    if(IsManagedGuildDelivery(request.transition.task)) {
        GuildDeliveryJob job;std::string blocker;
        if(!DecodeGuildDeliveryJob(request.transition.task.checkpoint.data,job,blocker) || !job.incomingMail)return {};
        // The existing native mail handler records the delivery's collection
        // in this same transaction. A bag increase alone cannot credit a parcel.
        proof+=" AND EXISTS(SELECT 1 FROM guild_society_supply_delivery d JOIN mail m ON m.id=d.mail_id"
            " WHERE d.delivery_id="+std::to_string(job.delivery)+" AND d.guild_id="+std::to_string(job.guild)+
            " AND d.goal_id="+SqlValue(job.goal)+" AND d.donor_guid="+std::to_string(job.donor)+
            " AND d.carrier_guid="+std::to_string(quote.actor)+" AND d.mail_id="+std::to_string(job.incomingMail)+
            " AND d.item_guid="+std::to_string(quote.guid)+" AND d.item_entry="+std::to_string(job.entry)+
            " AND d.quantity="+std::to_string(job.quantity)+" AND d.deposited_quantity=0 AND d.phase='carried'"
            " AND m.sender=d.donor_guid AND m.receiver=d.carrier_guid)";
    }
    return proof;
}
}
