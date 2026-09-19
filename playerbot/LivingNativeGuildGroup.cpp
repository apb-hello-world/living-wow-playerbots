#include "botpch.h"
#include "LivingNativeGuildGroup.h"
#include "LivingActivityCoordinator.h"
#include "LivingTrainingGroupPriority.h"
#include "PlayerbotSocialActionBroker.h"
#include <boost/property_tree/json_parser.hpp>
#include <sstream>
namespace LivingActivity {

namespace {
uint32_t GroupId(Player* p) {return p && p->GetGroup()?p->GetGroup()->GetId():0;}
uint32_t LeaderId(Player* p) {return p && p->GetGroup()?p->GetGroup()->GetLeaderGuid().GetCounter():0;}
bool SafeGroupActor(Player* p) {
    return p && p->GetPlayerbotAI() && !p->isRealPlayer() && p->IsInWorld() && p->IsAlive() &&
        !p->IsInCombat() && !p->IsBeingTeleported() && !p->IsTaxiFlying() && !p->GetTransport() &&
        !p->InBattleGround() && !p->InBattleGroundQueue() && !p->GetMap()->IsDungeon() &&
        !p->GetTradeData() && !p->IsNonMeleeSpellCasted(false) && !p->GetGroupInvite() &&
        !sPlayerbotSocialActionBroker.ReservedForPlayer(p->GetGUIDLow());
}
bool BotOnlyGroup(Player* p) {
    if(!p || !p->GetGroup())return true;
    if(p->GetGroup()->IsRaidGroup())return false;
    for(const auto& slot:p->GetGroup()->GetMemberSlots())
        if(!sPlayerbotAIConfig.IsInRandomAccountList(sObjectMgr.GetPlayerAccountIdByGUID(slot.guid)))return false;
    return true; // Includes offline human slots: account identity, not online pointers.
}
}
std::string EncodeGuildGroupQuote(const GuildGroupQuote& q) {
    return "{\"actor\":"+std::to_string(q.actor)+",\"coordinator\":"+std::to_string(q.coordinator)+
        ",\"group\":"+std::to_string(q.group)+",\"leader\":"+std::to_string(q.leader)+
        ",\"target_group\":"+std::to_string(q.targetGroup)+",\"target_leader\":"+std::to_string(q.targetLeader)+
        ",\"change\":\""+q.change+"\"}";
}
bool DecodeGuildGroupQuote(const std::string& text,GuildGroupQuote& q) {
    q={};if(text.size()>1024)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(text);boost::property_tree::read_json(in,p);
        if(p.size()!=7)return false;
        q.actor=p.get<uint32_t>("actor");q.coordinator=p.get<uint32_t>("coordinator");
        q.group=p.get<uint32_t>("group");q.leader=p.get<uint32_t>("leader");
        q.targetGroup=p.get<uint32_t>("target_group");q.targetLeader=p.get<uint32_t>("target_leader");q.change=p.get<std::string>("change");
        return q.actor && q.coordinator && ((q.group==0)==(q.leader==0)) &&
            ((q.targetGroup==0)==(q.targetLeader==0)) &&
            ((q.change=="leave" && q.group) ||
             (q.change=="join" && q.actor!=q.coordinator && !q.group && (!q.targetGroup || q.targetLeader==q.coordinator)) ||
             (q.change=="leader" && q.actor!=q.coordinator && q.group && q.group==q.targetGroup && q.leader==q.actor));
    } catch(const std::exception&) {q={};return false;}
}
bool PlanNativeGuildGroup(Player& actor,const Task& task,uint32_t coordinator,GuildGroupQuote& q,std::string& why) {
    q={};GuildEventCommitment event;
    if(!ValidateNativeGuildEventTask(actor,task,why) || !DecodeGuildEventCommitment(task.checkpoint.data,event))return false;
    auto* target=sRandomPlayerbotMgr.GetPlayerBot(coordinator);
    if(!SafeGroupActor(&actor) || !SafeGroupActor(target) || !BotOnlyGroup(&actor) || !BotOnlyGroup(target)) {
        why="guild_event_group_safety_or_human_commitment";return false;
    }
    GuildEventAcceptance accepted;
    if(!ReadNativeGuildEventAcceptance(*target,event,accepted,why) ||
        !(why=GuildEventAcceptanceBlocker(event,coordinator,accepted)).empty())return false;
    const auto current=CharacterDatabase.Query(("SELECT executor_guid FROM guild_society_event WHERE event_id="+
        SqlValue(event.event)+" AND revision="+std::to_string(event.eventRevision)).c_str());
    if(!current || current->Fetch()[0].GetUInt32()!=coordinator){why="guild_event_coordinator_changed";return false;}
    q.actor=actor.GetGUIDLow();q.coordinator=coordinator;q.group=GroupId(&actor);q.leader=LeaderId(&actor);
    q.targetGroup=GroupId(target);q.targetLeader=LeaderId(target);
    if(q.group && q.group==q.targetGroup && q.actor!=coordinator && q.leader==q.actor)q.change="leader";
    else if(q.actor==coordinator) {
        bool foreign=false;
        if(actor.GetGroup())for(const auto& slot:actor.GetGroup()->GetMemberSlots()) {
            const auto r=CharacterDatabase.Query(("SELECT accepted_revision FROM guild_society_rsvp WHERE event_id="+
                SqlValue(event.event)+" AND character_guid="+std::to_string(slot.guid.GetCounter())+" AND response='accepted'").c_str());
            if(!r || r->Fetch()[0].GetUInt32()!=event.eventRevision)foreign=true;
        }
        if(q.group && !foreign && q.leader!=coordinator){why="guild_event_leader_handoff_pending";return false;}
        if(q.group && foreign)q.change="leave";
    } else if(q.group!=q.targetGroup || !q.group) {
        if(q.targetGroup && (q.targetLeader!=coordinator || target->GetGroup()->IsFull())) {
            why="guild_event_coordinator_group_unavailable";return false;
        }
        q.change=q.group?"leave":"join";
    }
    if(q.change=="join" && LivingWowDeferBotPartyForTraining(&actor,target)) {
        why="guild_event_class_training_preparation_required";return false;
    }
    why.clear();return true; // Empty change means membership is already suitable.
}
bool NativeGuildGroup::ValidateNative(Player& actor,const OperationRequest& request,std::string& why) {
    GuildGroupQuote fresh,encoded;
    if(request.transition.task.actor!=quote.actor || request.transition.task.checkpoint.step!="guild_event_group" ||
        !DecodeGuildGroupQuote(request.beforeState,encoded) || EncodeGuildGroupQuote(encoded)!=EncodeGuildGroupQuote(quote) ||
        !PlanNativeGuildGroup(actor,request.transition.task,quote.coordinator,fresh,why))return false;
    if(EncodeGuildGroupQuote(fresh)!=EncodeGuildGroupQuote(quote)){why="guild_event_membership_snapshot_changed";return false;}
    return true;
}
NativeObservation InspectNativeGuildGroup(Player& actor,const GuildGroupQuote& q) {
    NativeObservation result;auto* target=sRandomPlayerbotMgr.GetPlayerBot(q.coordinator);
    const auto group=GroupId(&actor),leader=LeaderId(&actor);
    result.afterState="{\"group\":"+std::to_string(group)+",\"leader\":"+std::to_string(leader)+'}';
    const bool satisfied=q.change=="leave"?!group:target && group && group==GroupId(target) && leader==q.coordinator;
    const auto sql="SELECT COALESCE(MAX(m.groupId),0),COALESCE(MAX(g.leaderGuid),0) FROM group_member m"
        " LEFT JOIN `groups` g ON g.groupId=m.groupId WHERE m.memberGuid="+std::to_string(q.actor);
    const auto saved=CharacterDatabase.Query(sql.c_str());
    if(!saved || saved->GetFieldCount()!=2){result.evidence="guild_event_native_membership_read_pending";return result;}
    const auto* f=saved->Fetch();
    if(f[0].GetUInt32()!=group || f[1].GetUInt32()!=leader){result.evidence="guild_event_native_membership_save_pending";return result;}
    result.state=satisfied?OperationState::Verified:OperationState::Rejected;
    result.evidence=satisfied?"native_guild_group_membership_observed":"native_guild_group_intent_not_satisfied";
    result.nativeReference="guild_group:"+std::to_string(q.actor)+":"+std::to_string(group);
    return result;
}
NativeObservation NativeGuildGroup::ExecuteNative(Player& actor,const OperationRequest& request) {
    std::string why;
    if(!ValidateNative(actor,request,why)){NativeObservation r;r.state=OperationState::Rejected;r.evidence=why;return r;}
    auto* target=sRandomPlayerbotMgr.GetPlayerBot(quote.coordinator);
    if(quote.change=="leave")actor.RemoveFromGroup();
    else if(quote.change=="leader")actor.GetGroup()->ChangeLeader(target->GetObjectGuid());
    else {
        // Native session handlers retain faction, ignore, instance, capacity,
        // and invitation checks. Do not run legacy action-engine resets here.
        WorldPacket invite;invite<<actor.GetName()<<uint32(0);target->GetSession()->HandleGroupInviteOpcode(invite);
        if(actor.GetGroupInvite() && actor.GetGroupInvite()->GetLeaderGuid()==target->GetObjectGuid()) {
            WorldPacket accept;actor.GetSession()->HandleGroupAcceptOpcode(accept);
        }
    }
    return InspectNativeGuildGroup(actor,quote);
}
}
