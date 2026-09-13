#include "botpch.h"
#include "LivingNativeGuildMail.h"
#include "LivingNativeMailCollection.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityNativeContext.h"
#include "LivingServiceExecution.h"
#include "PlayerbotGuildSupplies.h"
#include "PlayerbotActionBroker.h"
#include "RandomPlayerbotMgr.h"
#include "Guilds/GuildMgr.h"
#include "Mails/Mail.h"
#include "strategy/values/ItemUsageValue.h"
namespace LivingActivity {
namespace {
bool SafeSender(Player& p) {
    return p.GetPlayerbotAI() && p.GetSession() && p.IsInWorld() && p.GetMap() && p.IsAlive() &&
        !p.IsBeingTeleported() && !ReadNativeSafety(p,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) &&
        p.IsStopped() && !p.GetMap()->IsDungeon() && !LivingServiceExecution::Busy(&p) && !p.GetTradeData();
}
bool MayDeposit(Guild& guild,uint32_t actor) {
    for(uint8_t tab=0;tab<guild.GetPurchasedTabs();++tab)
        if(guild.IsMemberHaveRights(actor,tab,GUILD_BANK_RIGHT_DEPOSIT_ITEM))return true;
    return false;
}
Player* EligibleRecipient(Player& sender,Guild& guild,Item& item,uint32_t actor,std::string& blocker) {
    blocker="guild_mail_no_deposit_recipient";
    auto* p=sRandomPlayerbotMgr.GetPlayerBot(actor);
    // A busy/dead bot can receive native mail without being interrupted. Its
    // queued collection/deposit still obeys all normal service safety checks.
    if(!p || p==&sender || !p->GetSession() || !p->GetPlayerbotAI() || p->isRealPlayer() ||
        !sPlayerbotAIConfig.IsInRandomAccountList(p->GetSession()->GetAccountId()) || !p->IsInWorld() ||
        p->GetGuildId()!=sender.GetGuildId() || p->GetTeam()!=sender.GetTeam() ||
        !guild.GetMemberSlot(p->GetObjectGuid()) || !MayDeposit(guild,actor))return nullptr;
    if(p->GetMailSize()>=50){blocker="guild_mail_recipient_mailbox_full";return nullptr;}
    if(sGuildSupplies.ReservedEntry(actor,item.GetEntry())) {
        blocker="guild_mail_recipient_pending_delivery";return nullptr;
    }
    // Capacity is an execution prerequisite, not a recipient identity right.
    // A full bank must not look like the guild has no deposit-capable member.
    // The recipient checks actual space before any native deposit operation.
    blocker.clear();return p;
}
bool EmptyParcelSlot(Player& actor,Item& item,uint32_t quantity,uint16_t& result) {
    auto fits=[&](uint8_t bag,uint8_t slot) {
        const auto position=uint16_t(uint16_t(bag)<<8|slot);
        if(actor.GetItemByPos(position) || !Player::IsInventoryPos(position))return false;
        ItemPosCountVec positions;
        // Conservative preflight; the native split revalidates the cloned
        // item's exact restrictions before it changes either stack.
        if(actor.CanStoreNewItem(bag,slot,positions,item.GetEntry(),quantity)!=EQUIP_ERR_OK ||
            positions.size()!=1 || positions[0].pos!=position || positions[0].count!=quantity)return false;
        result=position;return true;
    };
    for(uint8_t slot=INVENTORY_SLOT_ITEM_START;slot<INVENTORY_SLOT_ITEM_END;++slot)
        if(fits(INVENTORY_SLOT_BAG_0,slot))return true;
    for(uint8_t bag=INVENTORY_SLOT_BAG_START;bag<INVENTORY_SLOT_BAG_END;++bag) {
        auto* container=static_cast<Bag*>(actor.GetItemByPos(INVENTORY_SLOT_BAG_0,bag));
        if(container)for(uint8_t slot=0;slot<container->GetBagSize();++slot)if(fits(bag,slot))return true;
    }
    return false;
}
bool FreshDelivery(const GuildMailQuote& q) {
    auto rows=CharacterDatabase.PQuery("SELECT d.delivery_id FROM guild_society_supply_delivery d "
        "JOIN guild_society_supply_goal g ON g.goal_id=d.goal_id AND g.guild_id=d.guild_id "
        "JOIN guild_society_supply_execution e ON e.guild_id=d.guild_id WHERE d.delivery_id=%llu "
        "AND d.guild_id=%u AND d.goal_id='%s' AND d.donor_guid=%u AND d.carrier_guid=%u AND d.item_entry=%u "
        "AND d.quantity=%u AND d.mail_id=%u AND d.deposited_quantity=0 AND d.phase='carried' "
        "AND g.state='active' AND g.request_kind='item' AND g.item_entry=%u AND e.enabled=1",
        (unsigned long long)q.job.delivery,q.job.guild,q.job.goal.c_str(),q.job.donor,q.sender,q.job.entry,
        q.job.quantity,q.job.incomingMail,q.job.entry);
    return bool(rows);
}
}
bool PlanNativeGuildMail(Player& actor,const Task& task,GuildMailQuote& q,std::vector<ClaimConsumption>& uses,
    std::string& why,uint32_t selectedReceiver) {
    q={};uses.clear();auto reject=[&](const char* s){why=s;return false;};
    if(!sLivingActivityCoordinator.OnWorldThread() || task.actor!=actor.GetGUIDLow() || !SafeSender(actor))
        return reject("guild_mail_safety_or_arrival_wait");
    GuildDepositQuote carry;if(!sGuildSupplies.ReadManagedCarry(task,carry,why))return false;
    auto* guild=sGuildMgr.GetGuildById(carry.job.guild);
    if(!guild || actor.GetGuildId()!=carry.job.guild)return reject("guild_delivery_membership_changed");
    if(MayDeposit(*guild,task.actor))return reject("guild_mail_direct_deposit_available");
    if(carry.deposited || carry.amount!=carry.job.quantity)return reject("guild_mail_partial_delivery_requires_reconciliation");
    UnsettledClaimBatch claims;if(!sLivingActivityCoordinator.ReadTaskClaims(task.actor,task.id,task.revision,claims,why))return false;
    Item* item=nullptr;ResourceClaim held,postage;
    const auto privateItems=sPlayerbotActionBroker.ReservedItemsView();
    for(auto* candidate:actor.GetPlayerbotAI()->InventoryParseItems("all",IterateItemsMask::ITERATE_ITEMS_IN_BAGS)) {
        if(!candidate || candidate->GetEntry()!=carry.job.entry || (!carry.job.incomingMail && candidate->GetGUIDLow()!=carry.item))continue;
        if(candidate->GetCount()<carry.job.quantity || !candidate->CanBeTraded() || candidate->HasGeneratedLoot() || candidate->IsConjuredConsumable() ||
            ai::ItemUsageValue::IsNeededForQuest(&actor,candidate->GetEntry(),true) ||
            !privateItems || privateItems->Item(candidate->GetGUIDLow()))continue;
        ResourceClaim prospective;prospective.task=task.id;prospective.actor=task.actor;prospective.itemGuid=candidate->GetGUIDLow();
        prospective.itemEntry=carry.job.entry;prospective.quantity=carry.job.quantity;prospective.location="bags";prospective.state="held";
        if(!sGuildSupplies.AllowsManagedClaim(prospective))continue;
        uint32_t available=0;
        if(!sLivingActivityCoordinator.TaskResourceAvailability(task.id,task.revision,
            {task.actor,candidate->GetGUIDLow(),carry.job.entry,candidate->GetCount(),0,"bags"},available,why))return false;
        if(available<carry.job.quantity)continue;
        if(!item || (candidate->GetCount()==carry.job.quantity && item->GetCount()!=carry.job.quantity) ||
            ((candidate->GetCount()==carry.job.quantity)==(item->GetCount()==carry.job.quantity) && candidate->GetGUIDLow()<item->GetGUIDLow()))item=candidate;
    }
    if(!item)return reject("guild_mail_claimed_parcel_unavailable");
    q.job=carry.job;q.sender=task.actor;q.item=item->GetGUIDLow();
    if(item->GetCount()>carry.job.quantity) {
        q.sourceCount=item->GetCount();
        if(!EmptyParcelSlot(actor,*item,carry.job.quantity,q.splitPosition))return reject("guild_mail_split_capacity_required");
    }
    for(const auto& c:claims.claims) {
        if(c.location=="money") {
            if(!postage.id.empty() || c.state!="held" || c.copper!=30 || c.task!=task.id)
                return reject("guild_mail_postage_claim_requires_reconciliation");
            postage=c;
        } else if(c.itemGuid==item->GetGUIDLow()) {
            if(!held.id.empty() || c.state!="held" || c.location!="bags" || c.quantity!=carry.job.quantity ||
                !sGuildSupplies.AllowsManagedClaim(c))return reject("guild_mail_item_claim_requires_reconciliation");
            held=c;
        }
    }
    Player* receiver=nullptr;
    std::string recipientBlocker="guild_mail_no_deposit_recipient";
    auto eligible=[&](uint32_t guid) {
        std::string reason;auto* candidate=EligibleRecipient(actor,*guild,*item,guid,reason);
        // Keep the most actionable valid-recipient wait independent of roster
        // iteration order. Neither transient wait revokes the member's rights.
        if(!candidate && (reason=="guild_mail_recipient_pending_delivery" ||
            (reason=="guild_mail_recipient_mailbox_full" && recipientBlocker=="guild_mail_no_deposit_recipient")))
            recipientBlocker=reason;
        return candidate;
    };
    if(selectedReceiver)receiver=eligible(selectedReceiver);
    else {
        auto consider=[&](Player* member) {
            const auto guid=member->GetGUIDLow();
            if(!receiver || guid<receiver->GetGUIDLow())if(auto* candidate=eligible(guid))receiver=candidate;
        };
        guild->BroadcastWorker(consider,&actor); // Native online guild members, not a realm-wide bot scan.
    }
    if(!receiver)return reject(recipientBlocker.c_str());
    uint32_t money=0;
    if(!sLivingActivityCoordinator.TaskResourceAvailability(task.id,task.revision,{task.actor,0,0,0,actor.GetMoney(),"money"},money,why))return false;
    if(money<30)return reject("guild_mail_insufficient_unreserved_postage");
    const auto mailbox=NativeNearbyMailbox(actor);if(!mailbox)return reject("guild_mailbox_send_travel_required");
    q.job=carry.job;q.sender=task.actor;q.receiver=receiver->GetGUIDLow();q.item=item->GetGUIDLow();q.moneyBefore=actor.GetMoney();
    q.postage=30;q.delay=actor.GetSession()->GetAccountId()==receiver->GetSession()->GetAccountId()?0:sWorld.getConfig(CONFIG_UINT32_MAIL_DELIVERY_DELAY);
    q.bagBefore=actor.GetItemCount(q.job.entry,false);q.totalBefore=actor.GetItemCount(q.job.entry,true);q.position=item->GetPos();q.mailbox=mailbox;
    if(!ValidGuildMailQuote(q))return reject("guild_mail_quote_invalid");
    if(!held.id.empty())uses.push_back({held,q.job.quantity});
    if(!postage.id.empty())uses.push_back({postage,30});
    why.clear();return true;
}
bool NativeGuildSendReservation::ValidatePurpose(Player& actor,const ReservationRequest& request,std::string& why) {
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(request.transition.task.id);
    GuildMailQuote q;std::vector<ClaimConsumption> existing;
    if(!saved || saved->actor!=actor.GetGUIDLow() || !IsManagedGuildDelivery(*saved) || !ValidateGuildDeliveryTask(*saved,why) ||
        saved->revision!=request.transition.expectedRevision || request.changes.empty() || request.changes.size()>2)return false;
    if(request.changes.size()==1 && request.changes[0].expectedRevision && request.changes[0].after.state=="released") {
        // Permission may be restored before a send. Release only this task's
        // still-unspent postage, allowing direct deposit without a leaked hold.
        UnsettledClaimBatch claims;
        if(!sLivingActivityCoordinator.ReadTaskClaims(saved->actor,saved->id,saved->revision,claims,why))return false;
        for(auto c:claims.claims)if(c.location=="money" && c.state=="held" && c.copper==30) {
            ++c.revision;c.state="released";
            if(SameResourceClaim(c,request.changes[0].after))return true;
        }
        why="guild_mail_postage_release_not_current";return false;
    }
    if(!PlanNativeGuildMail(actor,*saved,q,existing,why))return false;
    auto uses=existing;
    for(const auto& change:request.changes) {
        if(change.expectedRevision || change.after.revision!=1)return false;
        uses.push_back({change.after,change.after.copper?30:q.job.quantity});
    }
    if(!ExactGuildMailConsumption(*saved,q,uses)){why="guild_mail_exact_postage_and_parcel_required";return false;}
    if(!FreshDelivery(q)){why="guild_mail_delivery_changed";return false;}
    return true;
}
bool NativeGuildMail::ValidateNative(Player& actor,const OperationRequest& request,std::string& why) {
    const auto saved=sLivingActivityCoordinator.ReadSavedTask(request.transition.task.id);
    GuildMailQuote current;std::vector<ClaimConsumption> uses;
    if(!saved || request.beforeState!=EncodeGuildMailQuote(quote) || !ExactGuildMailConsumption(*saved,quote,request.consumption) ||
        !PlanNativeGuildMail(actor,*saved,current,uses,why,quote.receiver))return false;
    if(EncodeGuildMailQuote(current)!=EncodeGuildMailQuote(quote) || !ExactGuildMailConsumption(*saved,current,uses)) {
        why="guild_mail_native_quote_changed";return false;
    }
    if(!FreshDelivery(quote)){why="guild_mail_delivery_changed";return false;}
    why.clear();return true;
}
NativeObservation NativeGuildMail::ExecuteNative(Player& actor,const OperationRequest& request) {
    NativeObservation out;std::string why;
    if(!ValidateNative(actor,request,why)){out.state=OperationState::Rejected;out.evidence=why.empty()?"guild_mail_validation_rejected":why;return out;}
    if(!sGuildSupplies.BeginManagedMail(quote)){out.state=OperationState::Rejected;out.evidence="guild_mail_native_capture_unavailable";return out;}
    struct Capture {bool active=true;~Capture(){if(active)sGuildSupplies.EndManagedMail();}} capture;
    auto* receiver=sRandomPlayerbotMgr.GetPlayerBot(quote.receiver);
    auto* item=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,quote.item));
    if(quote.sourceCount) {
        actor.SplitItem(quote.position,quote.splitPosition,quote.job.quantity);
        auto* parcel=actor.GetItemByPos(quote.splitPosition);
        if(!parcel || parcel->GetGUIDLow()==quote.item || parcel->GetOwnerGuid()!=actor.GetObjectGuid() ||
            parcel->GetEntry()!=quote.job.entry || parcel->GetCount()!=quote.job.quantity ||
            item->GetCount()!=quote.sourceCount-quote.job.quantity ||
            actor.GetItemCount(quote.job.entry,false)!=quote.bagBefore) {
            out.evidence="native_guild_split_requires_reconciliation";return out;
        }
        item=parcel;
    }
    const auto sentItem=item->GetGUIDLow();
    actor.MoveItemFromInventory(item->GetBagSlot(),item->GetSlot(),true);actor.ModifyMoney(-int32_t(quote.postage));
    MailDraft draft("Guild supply delivery","For guild supply request "+quote.job.goal+". Please deposit these items in the guild bank.");
    draft.AddItem(item);draft.SendMailTo(MailReceiver(receiver),MailSender(&actor),MAIL_CHECK_MASK_HAS_BODY,quote.delay);
    const auto mailId=sGuildSupplies.EndManagedMail();capture.active=false;
    const auto* mail=receiver->GetMail(mailId);const auto* attached=receiver->GetMItem(sentItem);
    if(mailId)out.nativeReference="mail:"+std::to_string(mailId)+":item:"+std::to_string(sentItem);
    out.afterState="{\"mail\":"+std::to_string(mailId)+",\"sender\":"+std::to_string(quote.sender)+
        ",\"receiver\":"+std::to_string(quote.receiver)+",\"original_donor\":"+std::to_string(quote.job.donor)+
        ",\"item\":"+std::to_string(sentItem)+",\"money\":"+std::to_string(actor.GetMoney())+
        (quote.sourceCount?",\"source_item\":"+std::to_string(quote.item)+",\"source_remaining\":"+std::to_string(quote.sourceCount-quote.job.quantity):"")+'}';
    const auto* remaining=actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,quote.item));
    if(CharacterDatabase.HasOpenTransaction() && mail && attached && mail->sender==quote.sender &&
        mail->receiverGuid==receiver->GetObjectGuid() && !mail->money && !mail->COD && mail->items.size()==1 &&
        mail->items.front().item_guid==sentItem && mail->items.front().item_template==quote.job.entry &&
        attached->GetOwnerGuid()==receiver->GetObjectGuid() && attached->GetEntry()==quote.job.entry && attached->GetCount()==quote.job.quantity &&
        (quote.sourceCount?remaining && remaining->GetOwnerGuid()==actor.GetObjectGuid() && remaining->GetPos()==quote.position &&
            remaining->GetEntry()==quote.job.entry && remaining->GetCount()==quote.sourceCount-quote.job.quantity:!remaining) &&
        !actor.GetItemByGuid(ObjectGuid(HIGHGUID_ITEM,sentItem)) && actor.GetMoney()==quote.moneyBefore-quote.postage &&
        actor.GetItemCount(quote.job.entry,false)==quote.bagBefore-quote.job.quantity && actor.GetItemCount(quote.job.entry,true)==quote.totalBefore-quote.job.quantity) {
        sent={quote.receiver,sentItem,quote.job.entry,quote.job.quantity,0,"mail",mailId};
        deliveredAt=mail->deliver_time;expiresAt=mail->expire_time;
        out.state=OperationState::Verified;out.evidence="native_guild_parcel_postage_and_handoff_observed";out.mailedItem=sent;
    } else out.evidence="native_guild_mail_requires_reconciliation";
    return out;
}
std::string NativeGuildMail::PersistedNativeProof(Player&,const OperationRequest&,const Task& task) const {
    if(sent.nativeReference)return GuildMailNativeProof(quote,task,sent,deliveredAt,expiresAt);
    // A rejected preflight must prove that the original stack and postage are
    // still owned. No effect is inferred from a missing outgoing mail ID.
    return "SELECT "+SqlValue(task.id)+','+std::to_string(task.revision)+" FROM characters c JOIN character_inventory v ON v.guid=c.guid"
        " JOIN item_instance i ON i.guid=v.item WHERE c.guid="+std::to_string(quote.sender)+" AND c.money="+std::to_string(quote.moneyBefore)+
        " AND i.guid="+std::to_string(quote.item)+" AND i.owner_guid=c.guid AND i.itemEntry="+std::to_string(quote.job.entry)+
        " AND i.count="+std::to_string(GuildMailSourceCount(quote))+" AND NOT EXISTS (SELECT 1 FROM mail_items a WHERE a.item_guid=i.guid)";
}
}
