#include "botpch.h"
#include "LivingNativeCommissionMail.h"
#include "LivingNativeParcelSlot.h"
#include "LivingCommissionSettlement.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityNativeContext.h"
#include "LivingNativeMailCollection.h"
#include "LivingServiceExecution.h"
#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"
#include "strategy/values/ItemUsageValue.h"
#include "Mails/Mail.h"

namespace LivingActivity {
namespace {
bool AcceptedMailAgreement(const Task& task,const CommissionJob& job,std::string& why) {
    // A checkpoint flag is not native craft evidence or a customer's consent.
    auto rows=CharacterDatabase.PQuery("SELECT c.authoritative_payload FROM organic_economy_commission c "
        "WHERE c.commission_id='%s' AND c.bot_guid=%u AND c.player_guid=%u AND c.state IN ('crafting','ready','traveling') "
        "AND EXISTS (SELECT 1 FROM living_activity_transition t WHERE t.task_id='%s' AND t.task_revision=%llu "
        "AND t.code='commission_craft_verified') AND NOT EXISTS (SELECT 1 FROM living_activity_operation o "
        "WHERE o.task_id='%s' AND o.kind='commission_mail_send' AND o.state='verified')",
        job.agreement.id.c_str(),task.actor,job.agreement.recipient,task.id.c_str(),
        (unsigned long long)job.craftFinishedRevision,task.id.c_str());
    CommissionContract accepted;
    if(!rows || !DecodeCommissionContract(rows->Fetch()[0].GetCppString(),accepted,why) ||
        EncodeCommissionContract(accepted)!=EncodeCommissionContract(job.agreement)) {
        why="commission_agreement_or_craft_proof_changed";return false;
    }
    return true;
}
}
bool PlanNativeCommissionMail(Player& actor,const Task& task,CommissionMailQuote& q,
    std::vector<ClaimConsumption>& uses,std::string& why) {
    q={};uses.clear();auto reject=[&](const char* code){why=code;return false;};
    CommissionJob job;ProfessionJob recipe;
    if(!sLivingActivityCoordinator.OnWorldThread() || !Validate(task,why) || !IsCommissionJob(task) ||
        !ValidateCommissionTask(task,why) || task.actor!=actor.GetGUIDLow() || !task.accepted || task.mode!=Mode::Active ||
        !DecodeCommissionJob(task.checkpoint.data,job,why) || !job.craftFinishedRevision || job.agreement.delivery!="mail" ||
        !DecodeProfessionIntent(job.craft,recipe,why))return reject("commission_mail_task_invalid");
    if(!actor.GetPlayerbotAI() || !actor.GetSession() || !actor.IsInWorld() || !actor.GetMap())
        return reject("commission_mail_sender_unavailable");
    const auto safety=ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR));
    if(safety)return reject(NativeSafetyReason(safety));
    if(actor.GetMap()->IsDungeon())return reject("commission_mail_sender_in_dungeon");
    if(!actor.IsStopped())return reject("commission_mail_sender_moving");
    if(actor.GetTradeData())return reject("trade_in_progress");
    if(LivingServiceExecution::Busy(&actor))return reject(LivingServiceExecution::Blocker(&actor));
    if(!AcceptedMailAgreement(task,job,why))return false;
    const ObjectGuid recipient(HIGHGUID_PLAYER,job.agreement.recipient);
    auto customer=CharacterDatabase.PQuery("SELECT name,account FROM characters WHERE guid=%u",job.agreement.recipient);
    if(!customer)return reject("commission_mail_recipient_missing");
    if(actor.GetTeam()!=sObjectMgr.GetPlayerTeamByGUID(recipient))return reject("commission_mail_faction_changed");
    UnsettledClaimBatch claims;
    if(!sLivingActivityCoordinator.ReadTaskClaims(task.actor,task.id,task.revision,claims,why))return false;
    if(!claims.complete)return reject("commission_mail_claims_incomplete");
    ResourceClaim output,postage;uint64_t outputQuantity=0;
    for(const auto& c:claims.claims) {
        if(c.state!="held" || c.nativeReference)return reject("commission_mail_claim_requires_reconciliation");
        if(c.location=="bags" && c.itemEntry==recipe.outputEntry && c.quantity && c.quantity<=recipe.outputQuantity) {
            if(!output.id.empty() && output.itemGuid!=c.itemGuid)return reject("commission_mail_multiple_stacks_preparation_required");
            output=c;outputQuantity+=c.quantity;uses.push_back({c,uint32_t(c.quantity)});
        }
        else if(c.location=="money" && c.copper==30 && postage.id.empty())postage=c;
        else return reject("commission_mail_exact_parcel_preparation_required");
    }
    if(outputQuantity!=recipe.outputQuantity || uses.size()>15)return reject("commission_mail_exact_output_claims_required");
    auto* item=output.id.empty()?nullptr:actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,output.itemGuid));
    const auto privateItems=sPlayerbotActionBroker.ReservedItemsView();
    if(!item || item->GetOwnerGuid()!=actor.GetObjectGuid() || !Player::IsInventoryPos(item->GetPos()) ||
        item->GetEntry()!=recipe.outputEntry || item->GetCount()<recipe.outputQuantity || !item->CanBeTraded() ||
        item->HasGeneratedLoot() || item->IsConjuredConsumable() || item->GetUInt32Value(ITEM_FIELD_DURATION) ||
        sGuildSupplies.Reserved(item->GetGUIDLow()) || !privateItems || privateItems->Item(item->GetGUIDLow()) ||
        ai::ItemUsageValue::IsNeededForQuest(&actor,item->GetEntry(),true))return reject("commission_mail_claimed_output_unavailable");
    uint32_t available=0;
    if(!sLivingActivityCoordinator.TaskResourceAvailability(task.id,task.revision,
        {task.actor,item->GetGUIDLow(),item->GetEntry(),item->GetCount(),0,"bags"},available,why))return false;
    if(available<recipe.outputQuantity)return reject("commission_mail_output_reserved_elsewhere");
    if(item->GetCount()>recipe.outputQuantity) {
        // Another job's claim cannot remain on the GUID that is mailed. Do not
        // split a mixed-obligation stack until its other reservations clear.
        if(available!=item->GetCount())return reject("commission_mail_surplus_reserved_elsewhere");
        q.entry=item->GetEntry();q.quantity=recipe.outputQuantity;q.count=item->GetCount();
        if(!EmptyNativeParcelSlot(actor,*item,q.count-q.quantity,q.splitPosition))return reject("commission_mail_split_capacity_required");
        if((q.splitPosition>>8)!=INVENTORY_SLOT_BAG_0) {
            const auto* bag=actor.GetItemByPos(INVENTORY_SLOT_BAG_0,uint8_t(q.splitPosition>>8));
            if(!bag)return reject("commission_mail_split_bag_missing");
            q.splitBagGuid=bag->GetGUIDLow();
        }
    }
    if(!sLivingActivityCoordinator.TaskResourceAvailability(task.id,task.revision,{task.actor,0,0,0,actor.GetMoney(),"money"},available,why))return false;
    if(available<30)return reject("commission_mail_postage_unavailable");
    const auto mailbox=NativeNearbyMailbox(actor);if(!mailbox)return reject("commission_mailbox_travel_required");
    q.commission=job.agreement.id;q.sender=task.actor;q.receiver=job.agreement.recipient;q.cod=job.agreement.feeCopper;
    q.item=item->GetGUIDLow();q.entry=item->GetEntry();q.quantity=recipe.outputQuantity;q.count=item->GetCount();
    q.moneyBefore=actor.GetMoney();q.postage=30;q.position=item->GetPos();q.mailbox=mailbox;
    q.delay=actor.GetSession()->GetAccountId()==customer->Fetch()[1].GetUInt32()?0:sWorld.getConfig(CONFIG_UINT32_MAIL_DELIVERY_DELAY);
    if(!ValidCommissionMailQuote(q))return reject("commission_mail_quote_invalid");
    if(!postage.id.empty())uses.push_back({postage,30});
    why.clear();return true;
}
bool NativeCommissionReturnReservation::ValidatePurpose(Player& actor,const ReservationRequest& request,std::string& why) {
    guard=" AND 1=0";
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(request.transition.task.id);
    ProfessionHistory history;ResourceClaim expected;UnsettledClaimBatch claims;NativeResourceBalance native;
    if(!saved || saved->revision!=request.transition.expectedRevision || saved->phase!=Phase::Preparing ||
        request.transition.task.checkpoint.step!="commission_return_collect" || request.changes.size()!=1 ||
        !sLivingActivityCoordinator.ReadProfessionHistory(actor.GetGUIDLow(),saved->id,saved->revision,history,why) ||
        !ReturnedCommissionClaim(*saved,history,expected,why) ||
        !sLivingActivityCoordinator.ReadTaskClaims(actor.GetGUIDLow(),saved->id,saved->revision,claims,why))return false;
    const auto& change=request.changes.front();
    if(!claims.complete || !claims.claims.empty() || change.expectedRevision || !SameResourceClaim(change.after,expected) ||
        !ReadNativeMailBalance(actor,expected,native)) {why="commission_return_attachment_changed";return false;}
    const auto* mail=actor.GetMail(uint32_t(expected.nativeReference));
    CommissionDeliveryProof proof;
    if(!InspectCommissionDelivery(*saved,history,proof,why))return false;
    if(!mail || mail->COD || mail->money || mail->sender!=proof.quote.receiver ||
        mail->subject!=CommissionMailSubject(proof.send) || mail->items.size()!=1 ||
        !(mail->checked&MAIL_CHECK_MASK_RETURNED)) {why="commission_return_envelope_changed";return false;}
    guard=ReturnedCommissionClaimGuard(*saved,history,expected);why.clear();return true;
}
bool NativeCommissionMailReservation::ValidatePurpose(Player& actor,const ReservationRequest& request,std::string& why) {
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(request.transition.task.id);
    CommissionMailQuote q;std::vector<ClaimConsumption> uses;
    if(!saved || saved->revision!=request.transition.expectedRevision || request.changes.size()!=1 ||
        !PlanNativeCommissionMail(actor,*saved,q,uses,why) || uses.empty() || uses.size()>15 ||
        std::any_of(uses.begin(),uses.end(),[](const ClaimConsumption& use){return use.before.copper!=0;}))return false;
    const auto& change=request.changes.front();
    if(change.expectedRevision || change.after.revision!=1){why="commission_mail_new_postage_claim_required";return false;}
    uses.push_back({change.after,30});
    if(!ExactCommissionMailConsumption(*saved,q,uses)){why="commission_mail_exact_postage_required";return false;}
    return true;
}
bool NativeCommissionMail::ValidateNative(Player& actor,const OperationRequest& request,std::string& why) {
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(request.transition.task.id);
    CommissionMailQuote fresh;std::vector<ClaimConsumption> uses;
    if(!saved || request.beforeState!=EncodeCommissionMailQuote(quote) ||
        !ExactCommissionMailConsumption(*saved,quote,request.consumption) ||
        !PlanNativeCommissionMail(actor,*saved,fresh,uses,why))return false;
    if(EncodeCommissionMailQuote(fresh)!=EncodeCommissionMailQuote(quote) || !ExactCommissionMailConsumption(*saved,fresh,uses)) {
        why="commission_mail_native_quote_changed";return false;
    }
    return true;
}
NativeObservation NativeCommissionMail::ExecuteNative(Player& actor,const OperationRequest& request) {
    NativeObservation out;std::string why;sent={};surplusItem=0;operation=request.transition.receipt;
    if(!ValidateNative(actor,request,why)) {out.state=OperationState::Rejected;out.evidence=why.empty()?"commission_mail_validation_rejected":why;return out;}
    if(!CharacterDatabase.HasOpenTransaction()){out.state=OperationState::Rejected;out.evidence="commission_mail_native_transaction_required";return out;}
    auto customer=CharacterDatabase.PQuery("SELECT name FROM characters WHERE guid=%u",quote.receiver);
    if(!customer){out.state=OperationState::Rejected;out.evidence="commission_mail_recipient_missing";return out;}
    if(quote.count>quote.quantity) {
        const auto total=actor.GetItemCount(quote.entry,false);
        actor.SplitItem(quote.position,quote.splitPosition,quote.count-quote.quantity);
        const auto* source=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,quote.item));
        const auto* extra=actor.GetItemByPos(quote.splitPosition);
        if(!source || !extra || extra->GetGUIDLow()==quote.item || source->GetOwnerGuid()!=actor.GetObjectGuid() ||
            extra->GetOwnerGuid()!=actor.GetObjectGuid() || source->GetPos()!=quote.position || source->GetEntry()!=quote.entry ||
            extra->GetEntry()!=quote.entry || source->GetCount()!=quote.quantity || extra->GetCount()!=quote.count-quote.quantity ||
            actor.GetItemCount(quote.entry,false)!=total) {
            out.evidence="commission_mail_split_requires_reconciliation";return out;
        }
        surplusItem=extra->GetGUIDLow();
    }
    NormalMailCapture capture;
    WorldPacket packet(CMSG_SEND_MAIL);
    packet<<ObjectGuid(quote.mailbox)<<customer->Fetch()[0].GetCppString()<<CommissionMailSubject(operation);
    packet<<std::string("Your commissioned item. The agreed fee is collected by the game's COD mail system.");
    packet<<uint32(0)<<uint32(0)<<uint8(1)<<uint8(0)<<ObjectGuid(HIGHGUID_ITEM,quote.item);
    packet<<uint32(0)<<quote.cod<<uint64(0)<<uint8(0);
    actor.GetSession()->HandleSendMail(packet);
    const auto* remaining=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,quote.item));
    if(!surplusItem && capture.Valid() && capture.Rows().empty() && remaining && remaining->GetOwnerGuid()==actor.GetObjectGuid() &&
        remaining->GetEntry()==quote.entry && remaining->GetCount()==quote.count && remaining->GetPos()==quote.position &&
        actor.GetMoney()==quote.moneyBefore) {
        out.state=OperationState::Rejected;out.evidence="commission_mail_native_handler_rejected";return out;
    }
    const auto* extra=surplusItem?actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,surplusItem)):nullptr;
    const bool surplusValid=!surplusItem || (extra && extra->GetOwnerGuid()==actor.GetObjectGuid() &&
        extra->GetPos()==quote.splitPosition && extra->GetEntry()==quote.entry && extra->GetCount()==quote.count-quote.quantity);
    if(surplusItem && surplusValid && capture.Valid() && capture.Rows().empty() && remaining &&
        remaining->GetOwnerGuid()==actor.GetObjectGuid() && remaining->GetEntry()==quote.entry &&
        remaining->GetCount()==quote.quantity && remaining->GetPos()==quote.position && actor.GetMoney()==quote.moneyBefore) {
        out.evidence="commission_mail_native_split_only";out.nativeReference="item:"+std::to_string(quote.item);
        out.afterState="{\"mail_not_sent\":true,\"surplus_item\":"+std::to_string(surplusItem)+
            ",\"surplus_count\":"+std::to_string(quote.count-quote.quantity)+'}';
        return out;
    }
    if(!capture.Valid() || capture.Rows().size()!=1 || !VerifyCommissionMailSent(quote,capture.Rows().front(),operation) || !surplusValid ||
        remaining || actor.GetMoney()!=quote.moneyBefore-quote.postage || !CharacterDatabase.HasOpenTransaction()) {
        out.evidence="commission_mail_native_custody_uncertain";return out;
    }
    sent=capture.Rows().front();out.state=OperationState::Verified;out.evidence="native_commission_parcel_and_postage_observed";
    out.nativeReference="mail:"+std::to_string(sent.id)+":item:"+std::to_string(sent.itemGuid);
    out.afterState="{\"mail\":"+std::to_string(sent.id)+",\"item\":"+std::to_string(sent.itemGuid)+
        ",\"receiver\":"+std::to_string(sent.receiver)+",\"cod\":"+std::to_string(sent.cod)+
        ",\"delivered_at\":"+std::to_string(sent.deliveredAt)+",\"expires_at\":"+std::to_string(sent.expiresAt)+
        ",\"postage\":"+std::to_string(quote.postage)+",\"customer_received\":false,\"fee_paid\":false"+
        (surplusItem?",\"surplus_item\":"+std::to_string(surplusItem)+",\"surplus_count\":"+std::to_string(quote.count-quote.quantity):std::string())+'}';
    return out;
}
std::string NativeCommissionMail::PersistedNativeProof(Player& actor,const OperationRequest&,const Task& task) const {
    if(sent.id)return CommissionMailSentProof(task,quote,sent,operation,surplusItem);
    // character_inventory.bag stores a container ITEM GUID, not the packed
    // inventory-position bag index. Rejected sends must verify either location.
    const auto* item=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,quote.item));
    const auto bag=item && item->GetContainer()?item->GetContainer()->GetGUIDLow():0;
    return "SELECT "+SqlValue(task.id)+','+std::to_string(task.revision)+" FROM characters c JOIN character_inventory v ON v.guid=c.guid"
        " JOIN item_instance i ON i.guid=v.item WHERE c.guid="+std::to_string(quote.sender)+" AND c.money="+std::to_string(quote.moneyBefore)+
        " AND i.guid="+std::to_string(quote.item)+" AND i.owner_guid=c.guid AND i.itemEntry="+std::to_string(quote.entry)+
        " AND i.count="+std::to_string(quote.count)+" AND v.bag="+std::to_string(bag)+" AND v.slot="+
        std::to_string(quote.position&255)+" AND NOT EXISTS (SELECT 1 FROM mail_items a WHERE a.item_guid=i.guid)";
}
}
