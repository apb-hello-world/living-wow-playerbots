#include "botpch.h"
#include "LivingNativeQuestReward.h"
#include "LivingNativeGuildEvent.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityNativeContext.h"
#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"
#include "strategy/values/ItemUsageValue.h"
#include <map>
namespace LivingActivity {
namespace {
WorldObject* RewardGiver(Player& actor,uint64_t id,uint32_t quest) {
    const ObjectGuid guid(id);WorldObject* giver=nullptr;
    if(guid.IsCreature()) {
        auto* c=actor.GetNPCIfCanInteractWith(guid,UNIT_NPC_FLAG_QUESTGIVER);
        if(c && !c->GetScriptId())giver=c;
    } else if(guid.IsGameObject()) {
        auto* g=actor.GetGameObjectIfCanInteractWith(guid,GAMEOBJECT_TYPE_QUESTGIVER);
        if(g && !g->GetScriptId())giver=g;
    }
    return giver && giver->HasInvolvedQuest(quest)?giver:nullptr;
}
bool QuoteAt(Player& actor,const Task& task,uint64_t giverId,uint32_t choice,QuestRewardQuote& q,std::string& why) {
    q={};auto reject=[&](const char* code){why=code;return false;};
    GuildEventCommitment event;
    if(!IsGuildEventCommitment(task) || !ValidateNativeGuildEventTask(actor,task,why) ||
        !DecodeGuildEventCommitment(task.checkpoint.data,event) || event.kind!="quest")return false;
    const auto active=CharacterDatabase.Query(("SELECT e.state FROM guild_society_event e JOIN guild_society_event_participant p"
        " ON p.event_id=e.event_id AND p.revision=e.revision WHERE e.event_id="+SqlValue(event.event)+
        " AND e.revision="+std::to_string(event.eventRevision)+" AND p.character_guid="+std::to_string(actor.GetGUIDLow())+
        " AND p.began_incomplete=1 AND e.ends_at>"+std::to_string(time(nullptr))).c_str());
    if(!active || active->Fetch()[0].GetCppString()!="active")return reject("quest_reward_event_not_active");
    const auto* quest=sObjectMgr.GetQuestTemplate(event.target);
    if(!quest || quest->IsRepeatable() || quest->GetRewSpell() || quest->GetRewSpellCast() ||
        quest->GetQuestCompleteScript() || quest->GetRewMailTemplateId() || quest->GetRewHonorableKills() || quest->GetCharTitleId())
        return reject("quest_reward_script_or_special_effect_unsupported");
    for(unsigned i=0;i<QUEST_SOURCE_ITEM_IDS_COUNT;++i)if(quest->ReqSourceId[i])
        return reject("quest_reward_source_item_cleanup_unsupported");
    if(!actor.GetPlayerbotAI() || !actor.IsInWorld() || ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) ||
        actor.GetTradeData() || actor.IsNonMeleeSpellCasted(false) || actor.GetMap()->IsDungeon())return reject("quest_reward_safety_pause");
    if(actor.GetQuestRewardStatus(event.target) || actor.GetQuestStatus(event.target)!=QUEST_STATUS_COMPLETE)
        return reject("quest_reward_native_completion_required");
    if(!RewardGiver(actor,giverId,event.target))return reject("quest_reward_giver_travel_required");
    if(choice>=QUEST_REWARD_CHOICES_COUNT || (quest->GetRewChoiceItemsCount() && !quest->RewChoiceItemId[choice]) ||
        (!quest->GetRewChoiceItemsCount() && choice) || !actor.CanRewardQuest(quest,choice,false))
        return reject("quest_reward_native_eligibility_or_capacity");
    q.actor=actor.GetGUIDLow();q.quest=event.target;q.giver=giverId;q.choice=choice;
    q.money=actor.GetMoney();q.level=actor.GetLevel();q.xp=actor.GetUInt32Value(PLAYER_XP);
    q.moneyDelta=quest->GetRewOrReqMoney();
    if(actor.GetLevel()>=actor.GetMaxAttainableLevel())
        q.moneyDelta+=int32(quest->GetRewMoneyMaxLevel()*sWorld.getConfig(CONFIG_FLOAT_RATE_DROP_MONEY));
    std::map<uint32_t,QuestRewardItem> items;
    for(unsigned i=0;i<QUEST_ITEM_OBJECTIVES_COUNT;++i)if(quest->ReqItemId[i]) {
        auto& item=items[quest->ReqItemId[i]];item.entry=quest->ReqItemId[i];item.consume+=quest->ReqItemCount[i];
    }
    auto output=[&](uint32_t entry,uint32_t count){if(entry && count){auto& item=items[entry];item.entry=entry;item.gain+=count;}};
    if(quest->GetRewChoiceItemsCount())output(quest->RewChoiceItemId[choice],quest->RewChoiceItemCount[choice]);
    for(unsigned i=0;i<quest->GetRewItemsCount();++i)output(quest->RewItemId[i],quest->RewItemCount[i]);
    const auto protection=sLivingActivityCoordinator.ResourceReservations().Inspect();
    const auto trade=sPlayerbotActionBroker.ReservedItemsView();const auto supplies=sGuildSupplies.ReservedItemsView();
    if(!protection || !protection->ready || !trade || !supplies)return reject("quest_reward_resource_snapshot_required");
    if(q.moneyDelta<0 && protection->UnreservedMoney(q.actor,q.money)<uint64_t(-q.moneyDelta))return reject("quest_reward_money_committed");
    for(auto& row:items) {
        auto& item=row.second;item.before=actor.GetItemCount(item.entry,true);
        if(item.consume && (protection->HasUncertainItem(q.actor,item.entry) || supplies->Entry(q.actor,item.entry)))
            return reject("quest_reward_items_committed");
        q.items.push_back(item);
    }
    // Native turn-in selects stacks itself. Reject any protected matching input
    // rather than pretend an unreserved quantity identifies its chosen GUID.
    unsigned inspected=0;
    for(unsigned bank=0;bank!=2;++bank)for(auto* item:actor.GetPlayerbotAI()->InventoryParseItems("all",bank?
        IterateItemsMask::ITERATE_ITEMS_IN_BANK:IterateItemsMask::ITERATE_ITEMS_IN_BAGS)) {
        if(++inspected>256)return reject("quest_reward_inventory_snapshot_limit");
        const auto found=items.find(item->GetEntry());
        if(found!=items.end() && found->second.consume &&
            (item->IsInTrade() || protection->ProtectedItem(item->GetGUIDLow()) || trade->Item(item->GetGUIDLow()) || supplies->Item(item->GetGUIDLow())))
            return reject("quest_reward_input_stack_committed");
    }
    if(!ValidQuestRewardQuote(q))return reject("quest_reward_quote_invalid");
    why.clear();return true;
}
std::vector<uint32_t> Counts(Player& actor,const QuestRewardQuote& q) {
    std::vector<uint32_t> result;for(const auto& item:q.items)result.push_back(actor.GetItemCount(item.entry,true));return result;
}
}
bool PlanNativeQuestReward(Player& actor,const Task& task,QuestRewardQuote& q,std::string& why) {
    q={};GuildEventCommitment event;
    if(!actor.GetPlayerbotAI() || !sLivingActivityCoordinator.OnWorldThread() ||
        !DecodeGuildEventCommitment(task.checkpoint.data,event) || event.kind!="quest") {why="quest_reward_saved_quest_required";return false;}
    const auto* quest=sObjectMgr.GetQuestTemplate(event.target);if(!quest){why="quest_reward_unknown_quest";return false;}
    // Preserve native gearing preference: equip upgrade, then another useful
    // reward, then stable index. No automatic equipping inside reward execution.
    uint32_t choice=0;int best=-1;
    for(unsigned i=0;i<quest->GetRewChoiceItemsCount();++i) {
        auto use=actor.GetPlayerbotAI()->GetAiObjectContext()->GetValue<ai::ItemUsage>("item usage",std::to_string(quest->RewChoiceItemId[i]))->Get();
        const int score=use==ai::ItemUsage::ITEM_USAGE_EQUIP?3:use==ai::ItemUsage::ITEM_USAGE_BAD_EQUIP?2:use!=ai::ItemUsage::ITEM_USAGE_NONE?1:0;
        if(score>best){best=score;choice=i;}
    }
    why="quest_reward_giver_travel_required";
    for(auto guid:actor.GetPlayerbotAI()->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("nearest npcs no los")->Get())
        if(RewardGiver(actor,guid.GetRawValue(),event.target))return QuoteAt(actor,task,guid.GetRawValue(),choice,q,why);
    return false;
}
bool NativeQuestReward::ValidateNative(Player& actor,const OperationRequest& r,std::string& why) {
    QuestRewardQuote current;
    if(r.transition.task.checkpoint.step!="guild_quest_reward" || r.beforeState!=EncodeQuestRewardQuote(quote) ||
        !QuoteAt(actor,r.transition.task,quote.giver,quote.choice,current,why))return false;
    if(EncodeQuestRewardQuote(current)!=EncodeQuestRewardQuote(quote)){why="quest_reward_native_quote_changed";return false;}
    return true;
}
NativeObservation InspectNativeQuestReward(Player& actor,const Task& task,const QuestRewardQuote& q) {
    NativeObservation out;const bool rewarded=actor.GetQuestRewardStatus(q.quest);
    out.nativeReference="quest_reward:"+std::to_string(q.actor)+":"+std::to_string(q.quest);
    out.afterState="{\"rewarded\":"+std::string(rewarded?"true":"false")+",\"money\":"+std::to_string(actor.GetMoney())+
        ",\"level\":"+std::to_string(actor.GetLevel())+",\"xp\":"+std::to_string(actor.GetUInt32Value(PLAYER_XP))+'}';
    const auto saved=CharacterDatabase.Query(("SELECT rewarded FROM character_queststatus WHERE guid="+std::to_string(q.actor)+
        " AND quest="+std::to_string(q.quest)).c_str());
    if(!saved || bool(saved->Fetch()[0].GetUInt32())!=rewarded){out.evidence="quest_reward_native_save_pending";return out;}
    if(rewarded) {
        GuildEventCommitment event;
        if(!DecodeGuildEventCommitment(task.checkpoint.data,event)){out.evidence="quest_reward_saved_event_required";return out;}
        const auto evidence=CharacterDatabase.Query(("SELECT 1 FROM guild_society_activity_proof WHERE event_id="+SqlValue(event.event)+
            " AND revision="+std::to_string(event.eventRevision)+" AND character_guid="+std::to_string(q.actor)+
            " AND kind=2 AND entry="+std::to_string(q.quest)+" AND source_guid="+std::to_string(q.quest)).c_str());
        if(!evidence){out.evidence="quest_reward_saved_activity_proof_missing";return out;}
        if(!QuestRewardCountsMatch(q,actor.GetMoney(),Counts(actor,q))) {out.evidence="quest_reward_restart_resources_uncertain";return out;}
        out.state=OperationState::Verified;out.evidence="quest_reward_native_saved_completion";
    } else {out.state=OperationState::Rejected;out.evidence="quest_reward_native_effect_not_saved";}
    return out;
}
NativeObservation NativeQuestReward::ExecuteNative(Player& actor,const OperationRequest& r) {
    NativeObservation out;std::string why;
    if(!ValidateNative(actor,r,why)){out.state=OperationState::Rejected;out.evidence=why.empty()?"quest_reward_intent_invalid":why;return out;}
    const auto occurred=uint32(time(nullptr));
    WorldPacket packet(CMSG_QUESTGIVER_CHOOSE_REWARD,16);packet<<ObjectGuid(quote.giver)<<uint32(quote.quest)<<uint32(quote.choice);
    actor.GetSession()->HandleQuestgiverChooseRewardOpcode(packet);
    out.nativeReference="quest_reward:"+std::to_string(quote.actor)+":"+std::to_string(quote.quest);
    const bool rewarded=actor.GetQuestRewardStatus(quote.quest);
    out.afterState="{\"rewarded\":"+std::string(rewarded?"true":"false")+",\"money\":"+std::to_string(actor.GetMoney())+
        ",\"level\":"+std::to_string(actor.GetLevel())+",\"xp\":"+std::to_string(actor.GetUInt32Value(PLAYER_XP))+'}';
    if(rewarded && QuestRewardCountsMatch(quote,actor.GetMoney(),Counts(actor,quote))) {
        // Reward and activity evidence share the retained native transaction.
        // The ordinary callback remains useful, but cannot be the sole proof
        // across a crash after reward and before its next mailbox flush.
        GuildEventCommitment event;DecodeGuildEventCommitment(r.transition.task.checkpoint.data,event);
        // Authority was checked immediately before the atomic native handler.
        // Keep the historical fact even if policy/calendar changes before the
        // asynchronous save commits. This does not complete/cancel an event or
        // award participation credit; those validators still inspect its state.
        const auto sql="INSERT IGNORE INTO guild_society_activity_proof (event_id,revision,character_guid,kind,entry,source_guid,map_id,instance_id,occurred_at)"
            " VALUES ("+SqlValue(event.event)+','+std::to_string(event.eventRevision)+','+std::to_string(quote.actor)+
            ",2,"+std::to_string(quote.quest)+','+std::to_string(quote.quest)+','+std::to_string(actor.GetMapId())+','+
            std::to_string(actor.GetInstanceId())+','+std::to_string(occurred)+')';
        if(!CharacterDatabase.HasOpenTransaction() || !CharacterDatabase.Execute(sql.c_str())) {
            out.evidence="quest_reward_activity_proof_capture_failed";return out;
        }
        out.state=OperationState::Verified;out.evidence="quest_reward_native_effect_observed";
    } else out.evidence="quest_reward_native_effect_uncertain";
    return out;
}
std::string NativeQuestReward::PersistedNativeProof(Player& actor,const OperationRequest& request,const Task& after) const {
    const bool rewarded=actor.GetQuestRewardStatus(quote.quest);
    std::string proof="SELECT "+SqlValue(after.id)+','+std::to_string(after.revision)+" FROM characters c JOIN character_queststatus q ON q.guid=c.guid"
        " WHERE c.guid="+std::to_string(actor.GetGUIDLow())+" AND q.quest="+std::to_string(quote.quest)+
        " AND q.rewarded="+(rewarded?"1":"0")+" AND c.money="+std::to_string(actor.GetMoney())+
        " AND c.level="+std::to_string(actor.GetLevel())+" AND c.xp="+std::to_string(actor.GetUInt32Value(PLAYER_XP));
    for(const auto& item:quote.items)proof+=" AND (SELECT COALESCE(SUM(i.count),0) FROM character_inventory v JOIN item_instance i ON i.guid=v.item"
        " WHERE v.guid=c.guid AND i.owner_guid=c.guid AND i.itemEntry="+std::to_string(item.entry)+")="+
        std::to_string(actor.GetItemCount(item.entry,true));
    if(rewarded) {
        GuildEventCommitment event;DecodeGuildEventCommitment(request.transition.task.checkpoint.data,event);
        proof+=" AND EXISTS(SELECT 1 FROM guild_society_activity_proof a WHERE a.event_id="+SqlValue(event.event)+
            " AND a.revision="+std::to_string(event.eventRevision)+" AND a.character_guid=c.guid AND a.kind=2 AND a.entry="+
            std::to_string(quote.quest)+" AND a.source_guid="+std::to_string(quote.quest)+')';
    }
    return proof;
}
}
