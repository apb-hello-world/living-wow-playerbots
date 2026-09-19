#include "playerbot/playerbot.h"
#include "PlayerbotGuildEventExecutor.h"
#include "PlayerbotGuildGovernance.h"
#include "GuildEventExecutionPolicy.h"
#include "GuildActivityEvidence.h"
#include "GuildRouteProposal.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityScope.h"
#include "LivingNativeGuildEvent.h"
#include "PlayerbotRendezvousManager.h"
#include "PlayerbotSocialActionBroker.h"
#include "RandomPlayerbotMgr.h"
#include "TravelMgr.h"
#include "Guilds/GuildMgr.h"
#include "Globals/ObjectAccessor.h"
#include "strategy/values/TravelValues.h"
#include <algorithm>
#include <future>
#include <map>
#include <set>

using namespace ai;
using namespace livingguild;
namespace {
Player* Online(uint32 guid) { return sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER,guid)); }
GuildRouteEpoch RouteEpoch(Player* bot) {
    GuildRouteEpoch e;
    if(!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || !bot->GetPlayerbotAI() || !bot->GetGroup())return e;
    e.actor=bot->GetGUIDLow();e.guild=bot->GetGuildId();e.map=bot->GetMapId();e.instance=bot->GetInstanceId();
    e.actorGeneration=bot->GetPlayerbotAI()->GetActivityActorEpoch();
    e.mapGeneration=bot->GetPlayerbotAI()->GetActivityMapEpoch();
    e.groupIdentity=bot->GetGroup()->GetLivingActivityIdentity();
    e.groupRevision=bot->GetGroup()->GetLivingActivityRevision();return e;
}
bool Safe(Player* p,uint32 guild,bool allowDungeon=false) {
    return p && p->IsInWorld() && p->GetMap() && p->GetSession() && p->GetGuildId()==guild &&
        p->IsAlive() && !p->IsInCombat() && !p->IsBeingTeleported() && !p->IsTaxiFlying() &&
        !p->GetTransport() && !p->InBattleGround() && (allowDungeon||!p->GetMap()->IsDungeon());
}
bool HasUncommittedHuman(Player* bot,const std::set<uint32>& accepted) {
    if(!bot||!bot->GetGroup()) return false;
    // Native roster slots include offline humans; online references alone can
    // accidentally classify their party as bot-only during logout.
    for(const auto& slot:bot->GetGroup()->GetMemberSlots()) {
        if(accepted.count(slot.guid.GetCounter())) continue;
        if(!sPlayerbotAIConfig.IsInRandomAccountList(sObjectMgr.GetPlayerAccountIdByGUID(slot.guid))) return true;
    }
    return false;
}
struct CalendarEvent {
    std::string id,kind,state;
    uint32 guild=0,target=0,revision=0,starts=0,ends=0,minimum=0,maximum=0,coordinator=0,phaseAt=0,started=0,instance=0,completedMask=0;
};
struct DungeonObjective {
    struct Boss {uint32 entry=0,bit=0,order=0;};
    std::vector<Boss> bosses;
    uint32 required=0,final=0,level=0;
    uint32 map=0,entranceTeam=0;
    const AreaTrigger* entrance=nullptr;
    WorldPosition approach;
    bool valid=false,hasEntrance=false;
};
const DungeonObjective& DungeonFor(uint32 map) {
    // Realm metadata is immutable after startup; compute once on the world thread.
    static std::map<uint32,DungeonObjective> cache;
    auto found=cache.find(map);if(found!=cache.end()) return found->second;
    DungeonObjective d;d.map=map;const auto* info=sMapStore.LookupEntry(map);
    bool supported=info&&info->IsNonRaidDungeon();
    auto bounds=sObjectMgr.GetDungeonEncounterBoundsByMap(map);
    for(auto it=bounds.first;it!=bounds.second;++it) {
        const auto& encounter=it->second;const auto* dbc=encounter.dbcEntry;
        if(!dbc||dbc->Difficulty!=0) continue;
        const auto* creature=sObjectMgr.GetCreatureTemplate(encounter.creditEntry);
        if(encounter.creditType!=ENCOUNTER_CREDIT_KILL_CREATURE||dbc->encounterIndex>=32||!creature) {supported=false;continue;}
        const uint32 bit=uint32(1)<<dbc->encounterIndex;
        d.bosses.push_back({encounter.creditEntry,bit,dbc->encounterData});d.required|=bit;
        if(encounter.lastEncounterDungeon) d.final|=bit;
        d.level=std::max(d.level,uint32(creature->MaxLevel));
    }
    std::sort(d.bosses.begin(),d.bosses.end(),[](const DungeonObjective::Boss& a,const DungeonObjective::Boss& b){return a.order<b.order;});
    d.entrance=sObjectMgr.GetMapEntranceTrigger(map);
    if(d.entrance) if(const auto* trigger=sAreaTriggerStore.LookupEntry(d.entrance->entry)) {
        d.approach=WorldPosition(trigger->mapid,trigger->x,trigger->y,trigger->z);
        d.hasEntrance=true;
        if(const auto* area=d.approach.GetArea()) {
            if(area->zone) if(const auto* zone=sAreaStore.LookupEntry(area->zone)) area=zone;
            d.entranceTeam=area->team;
        }
    }
    d.valid=supported&&d.required&&d.final&&d.hasEntrance&&d.approach.isValid();
    return cache.emplace(map,std::move(d)).first->second;
}
bool EventSafe(Player* p,const CalendarEvent& e) {
    return Safe(p,e.guild,e.kind=="dungeon")&&(!p->GetMap()->IsDungeon()||
        (p->GetMapId()==e.target&&p->GetMap()->IsRegularDifficulty()&&
         (!e.instance||p->GetInstanceId()==e.instance)));
}
const char* ActiveRole(Player* p) {
    return PlayerbotAI::IsTank(p,false)?"tank":PlayerbotAI::IsHeal(p,false)?"healer":"damage";
}
bool DungeonReady(Player* p,const DungeonObjective& d) {
    if(!p||!d.valid||p->GetDifficulty()!=0||p->GetLevel()+3<d.level||p->GetLevel()>d.level+8) return false;
    if(p->GetMapId()!=d.map) {
        if((d.entranceTeam==AREATEAM_HORDE && p->GetTeam()==ALLIANCE)||
            (d.entranceTeam==AREATEAM_ALLY && p->GetTeam()==HORDE)) return false;
        if(WorldPosition(p).distance(d.approach)>10000) return false;
    }
    // Fail closed on unsupported attunements instead of scheduling an event
    // whose participants cannot enter. Native portal checks remain authoritative.
    if(p->GetLevel()<d.entrance->requiredLevel || d.entrance->conditionId ||
        (d.entrance->requiredQuest && !p->GetQuestRewardStatus(d.entrance->requiredQuest)) ||
        ((d.entrance->requiredItem||d.entrance->requiredItem2) &&
         !(d.entrance->requiredItem&&p->GetItemCount(d.entrance->requiredItem,false)) &&
         !(d.entrance->requiredItem2&&p->GetItemCount(d.entrance->requiredItem2,false)))) return false;
    // No phantom equipment or forced respec: use the current real build and gear.
    if(!p->GetItemByPos(INVENTORY_SLOT_BAG_0,EQUIPMENT_SLOT_MAINHAND)||
       !p->GetItemByPos(INVENTORY_SLOT_BAG_0,EQUIPMENT_SLOT_CHEST)) return false;
    for(uint8 slot=EQUIPMENT_SLOT_START;slot<EQUIPMENT_SLOT_END;++slot) {
        const auto* item=p->GetItemByPos(INVENTORY_SLOT_BAG_0,slot);if(!item) continue;
        const uint32 maximum=item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
        if(maximum&&item->GetUInt32Value(ITEM_FIELD_DURABILITY)*5<maximum) return false;
    }
    return true;
}
bool SavedEventState(const CalendarEvent& e,const char* next,uint32 coordinator) {
    const auto saved=CharacterDatabase.PQuery("SELECT state,revision,executor_guid FROM guild_society_event WHERE event_id='%s'",e.id.c_str());
    if(!saved)return false;
    const auto* row=saved->Fetch();
    return row[0].GetCppString()==next && row[1].GetUInt32()==e.revision && row[2].GetUInt32()==coordinator;
}
bool Transition(const CalendarEvent& e,const char* next,const char* reason,uint32 now,uint32 coordinator=0) {
    if(!CharacterDatabase.BeginTransaction()) return false;
    CharacterDatabase.PExecute("UPDATE guild_society_event SET state='%s',failure_reason='%s',executor_guid=%u,updated_at=%u,started_at=IF('%s'='active' AND started_at=0,%u,started_at),finished_at=IF('%s' IN ('failed','cancelled','completed'),%u,finished_at) WHERE event_id='%s' AND revision=%u AND state='%s'",
        next,reason,coordinator?coordinator:e.coordinator,now,next,now,next,now,e.id.c_str(),e.revision,e.state.c_str());
    if(!CharacterDatabase.CommitTransactionDirect())return false;
    // This core's transaction API can report dispatch success after a native
    // SQL failure. Only an independent saved-state read permits follow-up
    // grouping/travel or release; a failed read leaves the next poll to reconcile.
    return SavedEventState(e,next,coordinator?coordinator:e.coordinator);
}
// One actor, at most one native membership mutation. Joining after a successful
// leave is a NEW step: its map/group epoch must be inspected again, not reused
// from the permission that authorized leaving the old party.
const char* FormBotParticipant(const CalendarEvent& e,Player* coordinator,Player* member,
    const std::set<uint32>& accepted) {
    if(!EventSafe(coordinator,e) || !coordinator->GetPlayerbotAI() || coordinator->isRealPlayer() ||
        !EventSafe(member,e) || !member->GetPlayerbotAI() || member->isRealPlayer() ||
        !accepted.count(coordinator->GetGUIDLow()) || !accepted.count(member->GetGUIDLow()))
        return "guild_formation_participant_unavailable";
    if(HasUncommittedHuman(member,{}))return "guild_formation_human_party_protected";
    if(member==coordinator) {
        bool foreignMember=false;
        if(member->GetGroup())for(const auto& slot:member->GetGroup()->GetMemberSlots())
            if(!accepted.count(slot.guid.GetCounter()))foreignMember=true;
        if(!member->GetGroup() || (!foreignMember &&
            member->GetGroup()->GetLeaderGuid()==member->GetObjectGuid()))return "guild_formation_coordinator_ready";
        if(accepted.size()<2)return "guild_formation_roster_incomplete";
        uint32 other=*accepted.begin();if(other==member->GetGUIDLow())other=*accepted.rbegin();
        auto* requester=Online(other);if(!requester)return "guild_formation_requester_unavailable";
        member->GetPlayerbotAI()->DoSpecificAction("leave",Event("guild calendar","",requester),true);
        return member->GetGroup()?"guild_formation_leave_rejected":"guild_formation_group_changed";
    }
    if(coordinator->GetGroup() && member->GetGroup()==coordinator->GetGroup())return "guild_formation_joined";
    if(HasUncommittedHuman(coordinator,accepted))return "guild_formation_coordinator_human_party_protected";
    if(coordinator->GetGroup() && (coordinator->GetGroup()->IsRaidGroup() || coordinator->GetGroup()->IsFull() ||
        coordinator->GetGroup()->GetLeaderGuid()!=coordinator->GetObjectGuid()))return "guild_formation_coordinator_group_unavailable";
    if(member->GetGroup()) {
        member->GetPlayerbotAI()->DoSpecificAction("leave",Event("guild calendar","",coordinator),true);
        return member->GetGroup()?"guild_formation_leave_rejected":"guild_formation_group_changed";
    }
    member->GetPlayerbotAI()->DoSpecificAction("join",Event("create group","",coordinator),true);
    return member->GetGroup() && member->GetGroup()==coordinator->GetGroup()?
        "guild_formation_group_changed":"guild_formation_join_rejected";
}
}

struct PlayerbotGuildEventExecutor::State {
    struct Reservation { std::string event; uint32 revision=0,coordinator=0; bool active=false,moving=false; };
    struct Route { TravelDestination* destination=nullptr; WorldPosition* point=nullptr; };
    struct Job { std::string event;uint32 revision=0,guid=0;GuildRouteEpoch epoch;std::future<std::vector<GuildRouteProposal>> future; };
    std::map<uint32,Reservation> reservations;
    std::map<std::string,std::set<uint32>> rosters;
    std::map<uint32,Route> installed;
    std::map<std::string,uint32> routeRetry,inviteRetry;
    std::vector<Job> jobs;
    std::map<uint32,std::pair<Job,std::vector<GuildRouteProposal>>> readyRoutes;
    std::map<std::string,CalendarEvent> events;
    ActivityEvidenceQueue mailbox;
    std::vector<ActivityProof> pendingProofs;
    uint32 nextUpdate=0;

    void FlushProofs() {
        // Backpressure remains in the bounded mailbox rather than draining and
        // dropping evidence behind an unacknowledged write. Retry the original
        // native occurrence time, never a freshly invented completion timestamp.
        auto incoming=mailbox.Drain(64-pendingProofs.size());
        for(auto& proof:incoming) pendingProofs.push_back(std::move(proof));
        if(pendingProofs.empty()||!CharacterDatabase.BeginTransaction()) return;
        std::map<uint32,bool> authority;
        for(const auto& p:pendingProofs) {
            if(!authority.count(p.binding.guild)) {
                Guild* guild=sGuildMgr.GetGuildById(p.binding.guild);
                authority[p.binding.guild]=guild&&sGuildGovernance.Allows(guild,"events");
            }
            if(!authority[p.binding.guild]) continue;
            if(p.kind==1) {
                const auto& dungeon=DungeonFor(p.map);
                if(!dungeon.valid||std::none_of(dungeon.bosses.begin(),dungeon.bosses.end(),[&p](const DungeonObjective::Boss& b){return b.entry==p.entry;})) continue;
            }
            // A real accepted revision, current guild membership and a persisted
            // participation baseline are required. Delayed/stale callbacks cannot
            // resurrect cancelled work. The durable key deduplicates replay.
            CharacterDatabase.PExecute("INSERT IGNORE INTO guild_society_activity_proof (event_id,revision,character_guid,kind,entry,source_guid,map_id,instance_id,occurred_at) SELECT e.event_id,e.revision,%u,%u,%u,%llu,%u,%u,%u FROM guild_society_event e JOIN guild_society_rsvp r ON r.event_id=e.event_id AND r.character_guid=%u JOIN guild_member m ON m.guid=r.character_guid AND m.guildid=e.guild_id JOIN guild_society_event_participant p ON p.event_id=e.event_id AND p.character_guid=r.character_guid AND p.revision=e.revision WHERE e.event_id='%s' AND e.guild_id=%u AND e.revision=%u AND e.state='active' AND r.response='accepted' AND r.accepted_revision=e.revision AND e.started_at<=%u AND %u<e.ends_at AND ((e.event_type='quest' AND %u=2 AND e.target_id=%u AND p.began_incomplete=1) OR (e.event_type='dungeon' AND %u=1 AND e.target_id=%u AND %u>0 AND (e.activity_instance_id=0 OR e.activity_instance_id=%u)))",
                p.actor,p.kind,p.entry,static_cast<unsigned long long>(p.source),p.map,p.instance,p.occurred,p.actor,
                p.binding.event.c_str(),p.binding.guild,p.binding.revision,p.occurred,p.occurred,
                p.kind,p.entry,p.kind,p.map,p.instance,p.instance);
        }
        if(!CharacterDatabase.CommitTransactionDirect()) return;
        pendingProofs.erase(std::remove_if(pendingProofs.begin(),pendingProofs.end(),[](const ActivityProof& p){
            // Dispatch success is not a receipt. Read the entire native identity
            // (including occurrence time) before retiring this callback. A lost
            // read/commit leaves it queued and INSERT IGNORE makes replay safe.
            const auto saved=CharacterDatabase.PQuery("SELECT e.revision,e.state,EXISTS(SELECT 1 FROM guild_society_activity_proof a WHERE a.event_id=e.event_id AND a.revision=%u AND a.character_guid=%u AND a.kind=%u AND a.entry=%u AND a.source_guid=%llu AND a.map_id=%u AND a.instance_id=%u AND a.occurred_at=%u) FROM guild_society_event e WHERE e.event_id='%s'",
                p.binding.revision,p.actor,p.kind,p.entry,static_cast<unsigned long long>(p.source),p.map,p.instance,p.occurred,p.binding.event.c_str());
            if(!saved) return false;
            const auto* row=saved->Fetch();
            if(row[2].GetUInt32()) return true;
            if(row[0].GetUInt32()!=p.binding.revision || TerminalEvent(row[1].GetCppString())) {
                sLog.outError("Living guild native proof rejected after event change: event=%s revision=%u actor=%u kind=%u entry=%u source=%llu occurred=%u saved_revision=%u state=%s",
                    p.binding.event.c_str(),p.binding.revision,p.actor,p.kind,p.entry,
                    static_cast<unsigned long long>(p.source),p.occurred,row[0].GetUInt32(),row[1].GetCppString().c_str());
                return true;
            }
            return false;
        }),pendingProofs.end());
    }

    void Release(uint32 guid,const Reservation& old) {
        sPlayerbotRendezvousManager.CancelGuildEvent(guid,old.event,"calendar_event_released");
        auto route=installed.find(guid);
        Player* bot=Online(guid);
        if(route!=installed.end()) {
            if(bot&&bot->GetPlayerbotAI()) {
                TravelTarget* target=bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
                // Never clear an explicit human command or a replacement route.
                if(!sLivingActivityCoordinator.EffectEnforcementEnabled() && target&&target->GetDestination()==route->second.destination&&target->GetPosition()==route->second.point) {
                    target->SetForced(false);target->SetStatus(TravelStatus::TRAVEL_STATUS_EXPIRED);
                }
            }
            installed.erase(route);
        }
        readyRoutes.erase(guid);
    }

    void ObjectiveRoute(const CalendarEvent& e,Player* coordinator,uint32 now,uint32 completedMask=0) {
        if(!coordinator||!EventSafe(coordinator,e)||!coordinator->GetPlayerbotAI()||
            (e.kind=="quest"&&coordinator->GetQuestRewardStatus(e.target))) return;
        auto context=coordinator->GetPlayerbotAI()->GetAiObjectContext();
        TravelTarget* target=context->GetValue<TravelTarget*>("travel target")->Get();
        const auto installedRoute=installed.find(coordinator->GetGUIDLow());
        if(installedRoute!=installed.end()&&target->GetDestination()==installedRoute->second.destination&&
            target->GetPosition()==installedRoute->second.point&&target->IsActive()&&target->IsDestinationActive()) {
            // This accepted commitment owns the route, but still uses normal
            // guarded movement. Do not wait for the optional-action lottery.
            coordinator->GetPlayerbotAI()->DoSpecificAction("travel",Event("guild event objective","",coordinator),true);
            coordinator->GetPlayerbotAI()->DoSpecificAction("move to travel target",Event("guild event objective","",coordinator),true);
            return;
        }
        if(jobs.size()>=2||readyRoutes.count(coordinator->GetGUIDLow())||now<routeRetry[e.id]) return;
        for(const auto& job:jobs) if(job.event==e.id) return;
        routeRetry[e.id]=now+30;
        const uint32 purpose=e.kind=="dungeon"?uint32(TravelDestinationPurpose::Boss):
            coordinator->GetQuestStatus(e.target)==QUEST_STATUS_COMPLETE?
            uint32(TravelDestinationPurpose::QuestTaker):uint32(TravelDestinationPurpose::QuestAllObjective);
        // Snapshot all Player-backed values before leaving the world thread.
        const PlayerTravelInfo info(coordinator); const WorldPosition center(coordinator);
        const uint32 objective=e.target;const bool dungeon=e.kind=="dungeon";
        std::vector<int32> entries;
        if(dungeon) {
            for(const auto& boss:DungeonFor(objective).bosses) if(!(completedMask&boss.bit)) entries.push_back(int32(boss.entry));
        } else entries.push_back(int32(objective));
        const auto epoch=RouteEpoch(coordinator);if(!epoch.Valid())return;
        jobs.push_back({e.id,e.revision,coordinator->GetGUIDLow(),epoch,std::async(std::launch::async,[info,center,purpose,objective,dungeon,entries]() {
            std::vector<GuildRouteProposal> result;
            // Keep native encounter order; never route to an arbitrary grind
            // destination under a dungeon label. Copied IDs only on this worker.
            for(int32 entry:entries) for(auto* destination:sTravelMgr.GetDestinations(info,purpose,{entry},true,10000)) {
                auto* questDestination=dynamic_cast<QuestTravelDestination*>(destination);
                if(dungeon?!dynamic_cast<BossTravelDestination*>(destination):
                    (!questDestination||questDestination->GetQuestId()!=objective)) continue;
                std::list<uint8> chances={10,50,90};
                WorldPosition* point=destination->GetNextPoint(center,chances);
                if(point&&(!dungeon||point->getMapId()==objective)) {
                    GuildRouteProposal proposal;proposal.purpose=uint32(destination->GetPurpose());
                    proposal.entry=destination->GetEntry();proposal.quest=questDestination?questDestination->GetQuestId():0;
                    proposal.map=point->getMapId();proposal.x=point->getX();proposal.y=point->getY();proposal.z=point->getZ();
                    if(proposal.Valid())result.push_back(proposal);
                }
                if(result.size()>=32) return result;
            }
            return result;
        })});
    }

    // One world-thread route installation, separable from async completion and
    // calendar planning. The shared executor can invoke this under its actor's
    // current task grant; a worker result alone never supplies that grant.
    bool ApplyObjectiveRoute(const Job& job,const std::vector<GuildRouteProposal>& proposals) {
        auto owner=reservations.find(job.guid);Player* bot=Online(job.guid);
        if(owner==reservations.end() || owner->second.event!=job.event || owner->second.revision!=job.revision ||
            !owner->second.active || !bot || !FreshGuildRoute(job.epoch,RouteEpoch(bot)) ||
            !Safe(bot,bot->GetGuildId(),true) || !bot->GetPlayerbotAI() ||
            HasUncommittedHuman(bot,rosters[job.event]) ||
            (!sLivingActivityCoordinator.EffectEnforcementEnabled() &&
             sPlayerbotRendezvousManager.GetPartyActivityOwner(job.guid)!=PlayerbotRendezvousManager::PartyActivityOwner::guild_event) ||
            !CharacterDatabase.PQuery("SELECT event_id FROM guild_society_event WHERE event_id='%s' AND state='active' AND revision=%u AND executor_guid=%u AND (event_type<>'dungeon' OR (target_id=%u OR %u=0)) AND (activity_instance_id=0 OR activity_instance_id=%u OR %u=0)",job.event.c_str(),job.revision,job.guid,bot->GetMapId(),bot->GetMap()->IsDungeon()?1:0,bot->GetInstanceId(),bot->GetMap()->IsDungeon()?1:0))return false;
        const PlayerTravelInfo currentInfo(bot);
        for(const auto& proposal:proposals) {
            if(!proposal.Valid())continue;
            Route route;
            // Resolve current metadata locally; never retain a pointer supplied
            // by a worker across a map, login, roster or leader change.
            for(auto* destination:sTravelMgr.GetDestinations(currentInfo,proposal.purpose,
                {proposal.quest?int32(proposal.quest):proposal.entry},true,10000)) {
                auto* quest=dynamic_cast<QuestTravelDestination*>(destination);
                if(uint32(destination->GetPurpose())!=proposal.purpose || destination->GetEntry()!=proposal.entry ||
                    (quest?quest->GetQuestId():0)!=proposal.quest || !destination->IsActive(bot,currentInfo))continue;
                auto* point=destination->GetClosestPoint(WorldPosition(proposal.map,proposal.x,proposal.y,proposal.z));
                if(point && point->getMapId()==proposal.map && point->getX()==proposal.x &&
                    point->getY()==proposal.y && point->getZ()==proposal.z)route={destination,point};
                if(route.destination)break;
            }
            if(!route.destination)continue;
            auto context=bot->GetPlayerbotAI()->GetAiObjectContext();
            auto* previousFuture=context->GetValue<FutureDestinations*>("future travel destinations")->Get();
            if(previousFuture&&previousFuture->valid()) {
                if(previousFuture->wait_for(std::chrono::seconds(0))!=std::future_status::ready)return false;
                try {previousFuture->get();} catch(...) {}
            }
            TravelTarget* target=context->GetValue<TravelTarget*>("travel target")->Get();
            target->SetTarget(route.destination,route.point);
            if(!FreshGuildRoute(job.epoch,RouteEpoch(bot)) || target->GetDestination()!=route.destination ||
                target->GetPosition()!=route.point)return false;
            target->SetForced(false);target->SetConditions({});
            target->SetRelevance(199); // Population load shedding, not movement safety.
            target->SetStatus(TravelStatus::TRAVEL_STATUS_READY);
            context->GetValue<GuidPosition>("rpg target")->Reset();
            context->GetValue<bool>("travel target active")->Reset();
            installed[job.guid]=route;return true;
        }
        return false;
    }
};

PlayerbotGuildEventExecutor::PlayerbotGuildEventExecutor():state_(new State) {}
PlayerbotGuildEventExecutor::~PlayerbotGuildEventExecutor()=default;
PlayerbotGuildEventExecutor& PlayerbotGuildEventExecutor::instance() { static PlayerbotGuildEventExecutor value;return value; }
bool PlayerbotGuildEventExecutor::DungeonSupported(uint32 map) {return DungeonFor(map).valid;}
bool PlayerbotGuildEventExecutor::DungeonParticipantReady(Player* player,uint32 map) {return DungeonReady(player,DungeonFor(map));}
std::string PlayerbotGuildEventExecutor::ExecuteParticipant(const LivingActivity::Task& task,const LivingActivity::ActionContext& action) {
    using namespace LivingActivity;
    GuildEventCommitment definition;std::string why;
    if(!sLivingActivityCoordinator.OnWorldThread() || !sLivingActivityCoordinator.EffectEnforcementEnabled() ||
        !ExecutionScope::Matches(task,action) || !IsGuildEventCommitment(task) ||
        (task.phase!=Phase::Preparing && task.phase!=Phase::Traveling) ||
        !DecodeGuildEventCommitment(task.checkpoint.data,definition))return "guild_event_execution_scope_required";
    auto* bot=Online(task.actor);const auto found=state_->events.find(definition.event);
    if(!bot || found==state_->events.end() || !ValidateNativeGuildEventTask(*bot,task,why))
        return why.empty()?"guild_event_current_snapshot_required":why;
    const auto& event=found->second;
    if(EncodeGuildEventCommitment({event.id,event.kind,event.guild,event.revision,event.target,event.starts,event.ends})!=task.checkpoint.data)
        return "guild_event_current_revision_required";
    if(!SavedEventState(event,event.state.c_str(),event.coordinator))return "guild_event_snapshot_changed";
    const auto roster=state_->rosters.find(event.id);auto* coordinator=Online(event.coordinator);
    if(roster==state_->rosters.end() || !roster->second.count(task.actor) || !roster->second.count(event.coordinator) ||
        !EventSafe(bot,event) || !EventSafe(coordinator,event))return "guild_event_participant_safety_pause";
    const auto& accepted=roster->second;
    if(HasUncommittedHuman(bot,{}) || HasUncommittedHuman(coordinator,{}))return "guild_event_human_party_requires_session_authority";
    if(!sLivingActivityCoordinator.PermitEffects(*bot->GetPlayerbotAI(),
        {Mask(Effect::Group)|Mask(Effect::Movement)|Mask(Effect::TravelTarget),Lane::Managed,true},"guild event participant"))
        return "guild_event_execution_authority_changed";
    if(bot->GetGroup() && bot->GetGroup()==coordinator->GetGroup() && bot!=coordinator &&
        bot->GetGroup()->GetLeaderGuid()==bot->GetObjectGuid()) {
        bot->GetGroup()->ChangeLeader(coordinator->GetObjectGuid());return "guild_event_group_context_changed";
    }
    if(bot==coordinator && bot->GetGroup() && bot->GetGroup()->GetLeaderGuid()!=bot->GetObjectGuid() &&
        accepted.count(bot->GetGroup()->GetLeaderGuid().GetCounter()))return "guild_event_leader_handoff_pending";
    if(event.state=="forming" || event.state=="traveling") {
        const auto formation=std::string(FormBotParticipant(event,coordinator,bot,accepted));
        if(formation!="guild_formation_joined" && formation!="guild_formation_coordinator_ready")return formation;
        if(bot==coordinator)return "guild_event_roster_assembly_pending";
        return sLivingActivityCoordinator.ApproachGuildParticipant(task,action,event.coordinator);
    }
    if(event.state!="active")return "guild_event_phase_not_executable";
    if(!bot->GetGroup() || bot->GetGroup()!=coordinator->GetGroup())return "guild_event_roster_recovery_pending";
    if(bot!=coordinator)return sLivingActivityCoordinator.ApproachGuildParticipant(task,action,event.coordinator);
    if(event.kind=="dungeon")for(uint32 guid:accepted)
        if(!DungeonReady(Online(guid),DungeonFor(event.target)))return "guild_event_dungeon_preparation_required";
    const auto ready=state_->readyRoutes.find(task.actor);
    if(ready!=state_->readyRoutes.end()) {
        const bool applied=state_->ApplyObjectiveRoute(ready->second.first,ready->second.second);
        state_->readyRoutes.erase(ready);
        return applied?"guild_event_route_installed":"guild_event_route_revalidation_required";
    }
    state_->ObjectiveRoute(event,bot,uint32(time(nullptr)),event.completedMask);
    return "guild_event_objective_in_progress";
}
void PlayerbotGuildEventExecutor::RecordCredit(uint32 actor,uint32 guild,uint32 group,uint32 kind,
    uint32 entry,uint64_t source,uint32 map,uint32 instance,uint32 occurred) {
    state_->mailbox.Record(actor,guild,group,kind,entry,source,map,instance,occurred);
}
bool PlayerbotGuildEventExecutor::Reserved(uint32 guid) const { return state_->reservations.count(guid)!=0; }
bool PlayerbotGuildEventExecutor::OwnsMovement(uint32 guid) const {
    if(sLivingActivityCoordinator.EffectEnforcementEnabled())return false; // The acknowledged task owns it now.
    auto it=state_->reservations.find(guid);return it!=state_->reservations.end()&&it->second.moving;
}
bool PlayerbotGuildEventExecutor::CanRendezvous(uint32 guid,uint32 coordinator,const std::string& event) const {
    // The first rendezvous has no travel session yet. Authorize it from the
    // executor's current accepted roster, never from an old movement session.
    auto a=state_->reservations.find(guid),b=state_->reservations.find(coordinator);
    auto roster=state_->rosters.find(event);
    if(guid==coordinator||a==state_->reservations.end()||b==state_->reservations.end()||
        roster==state_->rosters.end()||!roster->second.count(guid)||!roster->second.count(coordinator)) return false;
    const auto& member=a->second;const auto& leader=b->second;
    if(member.event!=event||leader.event!=event||member.revision!=leader.revision||
        member.coordinator!=coordinator||leader.coordinator!=coordinator||
        !member.moving||!leader.moving||member.active||leader.active) return false;
    Player* bot=Online(guid);Player* organizer=Online(coordinator);
    Guild* guild=bot?sGuildMgr.GetGuildById(bot->GetGuildId()):nullptr;
    return guild&&bot&&organizer&&Safe(bot,bot->GetGuildId())&&Safe(organizer,bot->GetGuildId())&&
        bot->GetPlayerbotAI()&&!bot->isRealPlayer()&&
        bot->GetGuildId()==organizer->GetGuildId()&&organizer->GetGroup()&&
        bot->GetGroup()==organizer->GetGroup()&&
        organizer->GetGroup()->GetLeaderGuid()==organizer->GetObjectGuid()&&
        !HasUncommittedHuman(bot,roster->second)&&
        sGuildGovernance.Allows(guild,"events");
}
bool PlayerbotGuildEventExecutor::CanGroupWith(uint32 first,uint32 second) const {
    auto a=state_->reservations.find(first),b=state_->reservations.find(second);
    return (a==state_->reservations.end()&&b==state_->reservations.end())||
        (a!=state_->reservations.end()&&b!=state_->reservations.end()&&a->second.event==b->second.event&&a->second.revision==b->second.revision);
}
bool PlayerbotGuildEventExecutor::AllowsMovement(uint32 guid,const std::string& action) const {
    auto it=state_->reservations.find(guid);if(it==state_->reservations.end()) return true;
    const auto& r=it->second;if(!r.moving) return true;
    if(action=="follow") return r.active&&guid!=r.coordinator;
    if(action=="move to fish" || action=="fish" || action=="random recipe") return false;
    if(action=="move to travel target" || action=="travel") {
        auto route=state_->installed.find(guid);Player* bot=Online(guid);
        if(!r.active||guid!=r.coordinator||route==state_->installed.end()||!bot||!bot->GetPlayerbotAI()) return false;
        auto* target=bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
        return target&&target->GetDestination()==route->second.destination&&target->GetPosition()==route->second.point;
    }
    // Selection and reset actions also replace travel targets, even when they
    // don't directly call MoveTo. Combat, healing, loot and local quest use stay.
    return action.find("travel target")==std::string::npos && action!="travel" &&
        action!="move to rpg target" && action!="move random" && action!="progression move random" &&
        action!="go" && action!="follow chat shortcut";
}

void PlayerbotGuildEventExecutor::Update() {
    const uint32 now=uint32(time(nullptr));if(now<state_->nextUpdate) return;
    state_->nextUpdate=now+10;
    if(!sGuildGovernance.IsAvailable()) {
        state_->mailbox.Publish({});
        for(const auto& old:state_->reservations) state_->Release(old.first,old.second);
        state_->reservations.clear();return;
    }
    state_->FlushProofs();
    std::map<uint32,State::Reservation> previous=state_->reservations;
    state_->reservations.clear();
    state_->rosters.clear();
    state_->events.clear();
    std::map<uint32,ActivityBinding> bindings;
    auto events=CharacterDatabase.PQuery("SELECT event_id,guild_id,event_type,state,target_id,revision,scheduled_at,COALESCE(ends_at,scheduled_at+3600),minimum_members,maximum_members,executor_guid,updated_at,started_at,activity_instance_id FROM guild_society_event WHERE origin<>'legacy' AND state IN ('announced','forming','traveling','active') AND scheduled_at<=%u ORDER BY scheduled_at,event_id LIMIT 24",now+900);
    if(events) do {
        Field* f=events->Fetch();CalendarEvent e;
        e.id=f[0].GetString();e.guild=f[1].GetUInt32();e.kind=f[2].GetString();e.state=f[3].GetString();
        e.target=f[4].GetUInt32();e.revision=f[5].GetUInt32();e.starts=f[6].GetUInt32();e.ends=f[7].GetUInt32();
        e.minimum=f[8].GetUInt32();e.maximum=f[9].GetUInt32();e.coordinator=f[10].GetUInt32();
        e.phaseAt=f[11].GetUInt32();
        e.started=f[12].GetUInt32();e.instance=f[13].GetUInt32();
        if(!Id(e.id)||e.maximum>5||!e.minimum||e.minimum>e.maximum) continue;
        struct RememberEvent {State& state;CalendarEvent& event;~RememberEvent(){state.events[event.id]=event;}} remember{*state_,e};
        Guild* guild=sGuildMgr.GetGuildById(e.guild);
        if(!guild) {Transition(e,"cancelled","guild_unavailable",now);continue;}
        // Other shared entries remain useful manual appointments. They do not
        // claim bot attendance, reserve movement or award timer-only credit.
        if(e.kind!="quest"&&e.kind!="dungeon") {
            if(e.ends<=now) Transition(e,"cancelled","manual_calendar_window_ended",now);
            continue;
        }
        if(!sGuildGovernance.Allows(guild,"events")) {Transition(e,"cancelled","authority_revoked",now);continue;}
        if(e.ends<=now) {Transition(e,"failed","outcome_not_verified",now);continue;}
        if(!e.target||
           (e.kind=="quest"&&!sObjectMgr.GetQuestTemplate(e.target))||
           (e.kind=="dungeon"&&(!DungeonSupported(e.target)||e.minimum!=5||e.maximum!=5))) {
            // Do not generate a new failed event every tick; one terminal
            // diagnostic retains the original title/schedule and all RSVPs.
            if(now>=e.starts) Transition(e,"failed","objective_executor_unavailable",now);
            continue;
        }
        std::set<uint32> accepted,declined;
        auto responses=CharacterDatabase.PQuery("SELECT character_guid,response,accepted_revision FROM guild_society_rsvp WHERE event_id='%s'",e.id.c_str());
        if(responses) do {
            Field* r=responses->Fetch();const std::string response=r[1].GetCppString();
            if(response=="accepted"&&r[2].GetUInt32()==e.revision) accepted.insert(r[0].GetUInt32());
            else declined.insert(r[0].GetUInt32()); // No silent override of a no, maybe or renewal.
        } while(responses->NextRow());
        auto conflicts=CharacterDatabase.PQuery("SELECT DISTINCT r.character_guid FROM guild_society_rsvp r JOIN guild_society_event e ON e.event_id=r.event_id WHERE r.event_id<>'%s' AND r.response='accepted' AND r.accepted_revision=e.revision AND e.state IN ('announced','forming','traveling','active') AND e.scheduled_at<%u AND COALESCE(e.ends_at,e.scheduled_at+3600)>%u",e.id.c_str(),e.ends+900,e.starts>900?e.starts-900:0);
        std::set<uint32> unavailable;
        if(conflicts) do {unavailable.insert(conflicts->Fetch()[0].GetUInt32());} while(conflicts->NextRow());
        auto canUse=[&](Player* bot) {
            return EventSafe(bot,e)&&bot->GetPlayerbotAI()&&!bot->isRealPlayer()&&
                !unavailable.count(bot->GetGUIDLow())&&
                (!state_->reservations.count(bot->GetGUIDLow())||state_->reservations.at(bot->GetGUIDLow()).event==e.id)&&
                !sPlayerbotSocialActionBroker.ReservedForPlayer(bot->GetGUIDLow())&&
                !HasUncommittedHuman(bot,accepted)&&
                (e.kind=="dungeon"?DungeonReady(bot,DungeonFor(e.target)):
                    ((bot->GetQuestStatus(e.target)==QUEST_STATUS_INCOMPLETE ||
                        (e.state=="active"&&bot->GetQuestStatus(e.target)==QUEST_STATUS_COMPLETE))&&
                    !bot->GetQuestRewardStatus(e.target)))&&
                // A pre-existing dungeon is never appropriated for a new event.
                (!bot->GetMap()->IsDungeon()||e.state=="active"||e.state=="traveling");
        };
        auto roleCount=[&](const std::string& role) {
            uint32 count=0;for(uint32 guid:accepted) if(Player* p=Online(guid)) if(ActiveRole(p)==role) ++count;
            return count;
        };
        if(e.state=="announced"&&accepted.size()<e.maximum) {
            const auto candidateList=sRandomPlayerbotMgr.GetChatBotGuids();
            std::vector<uint32> candidates(candidateList.begin(),candidateList.end());
            std::sort(candidates.begin(),candidates.end());
            for(uint32 guid:candidates) {
                if(accepted.size()>=e.maximum) break;
                if(accepted.count(guid)||declined.count(guid)||!canUse(Online(guid))) continue;
                const char* role=ActiveRole(Online(guid));
                if(e.kind=="dungeon"&&roleCount(role)>=(std::string(role)=="damage"?3u:1u)) continue;
                if(!CharacterDatabase.BeginTransaction()) break;
                CharacterDatabase.PExecute("INSERT INTO guild_society_rsvp (event_id,character_guid,response,role,human,accepted_revision,updated_at) VALUES ('%s',%u,'accepted','%s',0,%u,%u)",e.id.c_str(),guid,role,e.revision,now);
                if(CharacterDatabase.CommitTransactionDirect()) {
                    const auto saved=CharacterDatabase.PQuery("SELECT response,accepted_revision FROM guild_society_rsvp WHERE event_id='%s' AND character_guid=%u",e.id.c_str(),guid);
                    if(saved && saved->Fetch()[0].GetCppString()=="accepted" && saved->Fetch()[1].GetUInt32()==e.revision)
                        accepted.insert(guid);
                }
            }
        }
        Player* coordinator=Online(e.coordinator);
        if(!e.coordinator) for(uint32 guid:accepted) if(canUse(Online(guid))&&
            (e.kind!="dungeon"||std::string(ActiveRole(Online(guid)))=="tank")) {coordinator=Online(guid);e.coordinator=guid;break;}
        // When the opted-in tank is human, a bot can still coordinate invitations.
        if(!e.coordinator) for(uint32 guid:accepted) if(canUse(Online(guid))) {coordinator=Online(guid);e.coordinator=guid;break;}
        bool rolesReady=e.kind!="dungeon"||(roleCount("tank")==1&&roleCount("healer")==1&&roleCount("damage")==3);
        if(e.kind=="dungeon") for(uint32 guid:accepted) {
            Player* member=Online(guid);
            if(!member||!member->IsAlive()||!DungeonReady(member,DungeonFor(e.target))) rolesReady=false;
        }
        state_->rosters[e.id]=accepted;
        const bool managed=sLivingActivityCoordinator.EffectEnforcementEnabled();
        if(managed)for(uint32 guid:accepted) {
            auto* member=Online(guid);
            if(member && member->GetPlayerbotAI() && !member->isRealPlayer())
                sLivingActivityCoordinator.AdmitGuildEvent(guid,{e.id,e.kind,e.guild,e.revision,e.target,e.starts,e.ends});
        }
        auto bindParticipants=[&]() {
            if(!coordinator||!coordinator->GetGroup()||HasUncommittedHuman(coordinator,accepted)) return;
            for(uint32 guid:accepted) {
                Player* member=Online(guid);
                if(member&&member->GetGuildId()==e.guild&&member->GetGroup()==coordinator->GetGroup())
                    bindings[guid]={e.id,e.revision,e.guild,coordinator->GetGroup()->GetId(),e.ends};
            }
        };
        // Combat pauses routing, not evidence collection. Publishing only from
        // the safe-movement branch would lose the very kills being measured.
        if(e.state=="active") bindParticipants();
        for(uint32 guid:accepted) {
            Player* bot=Online(guid);
            if(bot&&bot->GetPlayerbotAI()&&!bot->isRealPlayer()&&!unavailable.count(guid)&&
                !HasUncommittedHuman(bot,accepted)&&!state_->reservations.count(guid))
                state_->reservations[guid]={e.id,e.revision,e.coordinator,e.state=="active",now>=e.starts};
        }
        if(e.state=="announced") {
            // Reserve fifteen minutes early, but don't stop questing/group or
            // summon anybody until the actual scheduled start.
            if(now<e.starts) {
                continue;
            }
            if(!coordinator||!canUse(coordinator)||accepted.size()<e.minimum||!rolesReady) {
                if(EventFormationExpired(e.starts,now)) Transition(e,"failed","no_safe_roster",now);
                continue;
            }
            if(Transition(e,"forming","",now,e.coordinator)) {e.state="forming";e.phaseAt=now;}
            else continue;
        }
        if(!EventSafe(coordinator,e)||HasUncommittedHuman(coordinator,accepted)) {
            if(e.state!="active"&&EventFormationExpired(e.phaseAt,now)) Transition(e,"failed","coordinator_unavailable",now);
            continue;
        }
        if(e.state=="active") {
            bool reassemble=!coordinator->GetGroup();
            for(uint32 guid:accepted) {
                Player* member=Online(guid);
                if(member&&EventSafe(member,e)&&!HasUncommittedHuman(member,{})&&
                    member->GetGroup()!=coordinator->GetGroup()) reassemble=true;
            }
            // Restart recovery gets one state-scoped formation window, not
            // the date this event was originally put on the calendar. Saved
            // participation evidence is retained and no samples are backfilled.
            if(reassemble&&Transition(e,"traveling","roster_recovery",now)) {
                e.state="traveling";e.phaseAt=now;
                for(uint32 guid:accepted) {
                    auto r=state_->reservations.find(guid);if(r!=state_->reservations.end()) r->second.active=false;
                }
            }
        }
        if(e.state=="forming"||e.state=="traveling") {
            // Revalidate each native mutation; no generic invitation helper
            // that can silently convert a full five-player group into a raid.
            if(!managed)FormBotParticipant(e,coordinator,coordinator,accepted);
            for(uint32 guid:accepted) {
                if(managed)break; // Shared due queue alone performs participant mutations.
                Player* member=Online(guid);
                if(member==coordinator||!EventSafe(member,e)||unavailable.count(guid)) continue;
                if(coordinator->GetGroup()&&member->GetGroup()==coordinator->GetGroup()) continue;
                if(coordinator->GetGroup()&&(coordinator->GetGroup()->IsRaidGroup()||coordinator->GetGroup()->IsFull()||
                    coordinator->GetGroup()->GetLeaderGuid()!=coordinator->GetObjectGuid())) break;
                if(member->isRealPlayer()) {
                    const std::string key=e.id+":"+std::to_string(guid);
                    if(!member->GetGroup()&&!member->GetGroupInvite()&&now>=state_->inviteRetry[key]) {
                        state_->inviteRetry[key]=now+120;
                        WorldPacket invite;invite<<member->GetName();invite<<uint32(0);
                        coordinator->GetSession()->HandleGroupInviteOpcode(invite);
                    }
                    continue; // Only the human's normal accept opcode can join them.
                }
                FormBotParticipant(e,coordinator,member,accepted);
            }
            uint32 assembled=0;
            for(uint32 guid:accepted) {
                Player* member=Online(guid);
                if(!EventSafe(member,e)||!coordinator->GetGroup()||member->GetGroup()!=coordinator->GetGroup()) continue;
                if(e.kind=="dungeon"&&!DungeonReady(member,DungeonFor(e.target))) continue;
                if(member->IsWithinDistInMap(coordinator,60.0f)) ++assembled;
                else if(!managed&&!member->isRealPlayer()&&member->GetPlayerbotAI())
                    sPlayerbotRendezvousManager.Request(member,coordinator,"guild-event:"+e.id,false);
            }
            if(rolesReady&&assembled>=e.minimum&&assembled==accepted.size()) {
                if(!CharacterDatabase.BeginTransaction()) continue;
                for(uint32 guid:accepted) {
                    Player* member=Online(guid);if(!member) continue;
                    CharacterDatabase.PExecute("INSERT INTO guild_society_event_participant (event_id,character_guid,revision,began_incomplete) VALUES ('%s',%u,%u,%u) ON DUPLICATE KEY UPDATE began_incomplete=IF(revision=VALUES(revision),began_incomplete,VALUES(began_incomplete)),together_samples=IF(revision=VALUES(revision),together_samples,0),verified_at=IF(revision=VALUES(revision),verified_at,0),revision=VALUES(revision)",e.id.c_str(),guid,e.revision,member->GetQuestStatus(e.target)==QUEST_STATUS_INCOMPLETE?1:0);
                }
                if(!CharacterDatabase.CommitTransactionDirect()) continue;
                if(Transition(e,"active","",now)) {
                    e.state="active";if(!e.started) e.started=now;
                    for(uint32 guid:accepted) {
                        sPlayerbotRendezvousManager.CancelGuildEvent(guid,e.id,"calendar_event_started");
                        auto r=state_->reservations.find(guid);if(r!=state_->reservations.end()) r->second.active=true;
                    }
                }
            } else if(EventFormationExpired(e.phaseAt,now)) Transition(e,"failed","assembly_timeout",now);
        }
        if(e.state!="active") continue;
        // An unfinished quest member must own objective travel. Once the
        // current bot leader finishes, transfer only a fully bot-owned event
        // group, retaining the same event/revision and all earned evidence.
        if(e.kind=="quest" && coordinator->GetQuestStatus(e.target)!=QUEST_STATUS_INCOMPLETE &&
            !HasUncommittedHuman(coordinator,{}) && coordinator->GetGroup() &&
            coordinator->GetGroup()->GetLeaderGuid()==coordinator->GetObjectGuid()) {
            for(uint32 guid:accepted) {
                Player* next=Online(guid);
                if(next==coordinator || !EventSafe(next,e) || next->isRealPlayer() || !next->GetPlayerbotAI() ||
                    next->GetGroup()!=coordinator->GetGroup() || next->GetQuestStatus(e.target)!=QUEST_STATUS_INCOMPLETE) continue;
                if(!Transition(e,"active","quest_objective_leader_handoff",now,guid)) break;
                auto old=state_->reservations.find(e.coordinator);
                if(old!=state_->reservations.end()) state_->Release(e.coordinator,old->second);
                if(!managed)coordinator->GetGroup()->ChangeLeader(next->GetObjectGuid());
                coordinator=next;e.coordinator=guid;
                for(uint32 member:accepted) {
                    auto r=state_->reservations.find(member);
                    if(r!=state_->reservations.end()) r->second.coordinator=guid;
                }
                break;
            }
        }
        uint32 completedMask=0;
        std::map<uint32,uint32> proofMasks;
        std::set<uint32> questRewards;
        auto proofs=CharacterDatabase.PQuery("SELECT character_guid,kind,entry,instance_id,occurred_at FROM guild_society_activity_proof WHERE event_id='%s' AND revision=%u ORDER BY occurred_at,instance_id,character_guid LIMIT 512",e.id.c_str(),e.revision);
        if(proofs) do {
            Field* p=proofs->Fetch();const uint32 actor=p[0].GetUInt32(),kind=p[1].GetUInt32(),entry=p[2].GetUInt32(),instance=p[3].GetUInt32();
            if(!accepted.count(actor)||p[4].GetUInt32()<e.started) continue;
            if(e.kind=="quest"&&kind==2&&entry==e.target) questRewards.insert(actor);
            if(e.kind=="dungeon"&&kind==1&&instance) for(const auto& boss:DungeonFor(e.target).bosses) if(boss.entry==entry) {
                if(!e.instance) {
                    if(!CharacterDatabase.BeginTransaction()) break;
                    CharacterDatabase.PExecute("UPDATE guild_society_event SET activity_instance_id=%u WHERE event_id='%s' AND revision=%u AND state='active' AND activity_instance_id=0",instance,e.id.c_str(),e.revision);
                    if(!CharacterDatabase.CommitTransactionDirect()) break;
                    const auto saved=CharacterDatabase.PQuery("SELECT activity_instance_id,revision FROM guild_society_event WHERE event_id='%s'",e.id.c_str());
                    if(!saved || saved->Fetch()[1].GetUInt32()!=e.revision || saved->Fetch()[0].GetUInt32()!=instance)break;
                    e.instance=instance;
                }
                if(e.instance==instance) {proofMasks[actor]|=boss.bit;completedMask|=boss.bit;}
            }
        } while(proofs->NextRow());
        e.completedMask=completedMask;
        bindParticipants();
        // No direct MoveFollow calls on humans; native party/combat strategies
        // remain responsible for following, fighting and nearby quest use.
        auto participation=CharacterDatabase.PQuery("SELECT character_guid,began_incomplete,together_samples,verified_at FROM guild_society_event_participant WHERE event_id='%s' AND revision=%u",e.id.c_str(),e.revision);
        uint32 verified=0;
        if(participation) do {
            Field* p=participation->Fetch();const uint32 guid=p[0].GetUInt32();Player* member=Online(guid);
            if(!accepted.count(guid)) continue;
            if(p[3].GetUInt32()) {++verified;continue;}
            if(!member||!member->IsInWorld()||member->GetGuildId()!=e.guild||!coordinator->GetGroup()||member->GetGroup()!=coordinator->GetGroup()) continue;
            uint32 samples=p[2].GetUInt32();
            if(member->IsWithinDistInMap(coordinator,60.0f)&&samples<6) {
                ++samples;CharacterDatabase.PExecute("UPDATE guild_society_event_participant SET together_samples=%u WHERE event_id='%s' AND character_guid=%u AND revision=%u",samples,e.id.c_str(),guid,e.revision);
            }
            const uint32 mask=proofMasks[guid];uint32 bosses=0;
            for(uint32 bits=mask;bits;bits&=bits-1) ++bosses;
            const bool earned=e.kind=="quest"?(p[1].GetUInt32()!=0&&questRewards.count(guid)):
                (bosses>=std::min<size_t>(2,DungeonFor(e.target).bosses.size()));
            if(earned) {
                CharacterDatabase.PExecute("UPDATE guild_society_event_participant SET verified_at=%u WHERE event_id='%s' AND character_guid=%u AND revision=%u AND verified_at=0",now,e.id.c_str(),guid,e.revision);
                // Count from the next durable read, never from an uncommitted write.
            }
        } while(participation->NextRow());
        const bool objectiveDone=e.kind=="quest"||((completedMask&DungeonFor(e.target).required)==DungeonFor(e.target).required);
        // Minimum attendance is an admission rule, not permission to abandon
        // accepted participants after the first two quest rewards. Withdrawn
        // RSVPs leave this set explicitly; unfinished accepted members do not.
        if(verified>=e.minimum&&verified==accepted.size()&&objectiveDone) {
            if(!CharacterDatabase.BeginTransaction()) continue;
            CharacterDatabase.PExecute("UPDATE guild_society_event SET state='completed',failure_reason='%s',finished_at=%u,updated_at=%u WHERE event_id='%s' AND state='active' AND revision=%u",e.kind=="quest"?"quest_reward_verified":"dungeon_encounters_verified",now,now,e.id.c_str(),e.revision);
            CharacterDatabase.PExecute("INSERT IGNORE INTO guild_society_credit (guild_id,character_guid,source_id,source_type,earned_at) SELECT %u,p.character_guid,CONCAT('event:',p.event_id),'%s',p.verified_at FROM guild_society_event_participant p JOIN guild_member m ON m.guid=p.character_guid AND m.guildid=%u WHERE p.event_id='%s' AND p.revision=%u AND p.verified_at>0",e.guild,e.kind=="quest"?"verified_quest":"verified_dungeon",e.guild,e.id.c_str(),e.revision);
            if(CharacterDatabase.CommitTransactionDirect() && SavedEventState(e,"completed",e.coordinator)) for(uint32 guid:accepted) {
                bindings.erase(guid);
                auto owned=state_->reservations.find(guid);
                if(owned!=state_->reservations.end()&&owned->second.event==e.id) {
                    state_->Release(guid,owned->second);state_->reservations.erase(owned);previous.erase(guid);
                }
            }
        } else if(rolesReady) {if(!managed)state_->ObjectiveRoute(e,coordinator,now,completedMask);}
        else {
            auto owner=state_->reservations.find(e.coordinator);
            if(owner!=state_->reservations.end()) state_->Release(e.coordinator,owner->second);
        }
    } while(events->NextRow());
    state_->mailbox.Publish(std::move(bindings));

    for(const auto& old:previous) {
        auto current=state_->reservations.find(old.first);
        if(current==state_->reservations.end()||current->second.event!=old.second.event||current->second.revision!=old.second.revision)
            state_->Release(old.first,old.second);
    }
    // Never destroy a running std::async future on the world thread: its
    // destructor can wait. Cancelled jobs are drained once ready and discarded.
    for(auto it=state_->jobs.begin();it!=state_->jobs.end();) {
        if(it->future.wait_for(std::chrono::seconds(0))!=std::future_status::ready) {++it;continue;}
        std::vector<GuildRouteProposal> proposals;
        try {proposals=it->future.get();} catch(...) {proposals.clear();}
        if(sLivingActivityCoordinator.EffectEnforcementEnabled()) {
            if(state_->readyRoutes.size()<64)state_->readyRoutes[it->guid]={std::move(*it),std::move(proposals)};
        } else state_->ApplyObjectiveRoute(*it,proposals);
        it=state_->jobs.erase(it);
    }
    for(auto it=state_->routeRetry.begin();it!=state_->routeRetry.end();)
        if(uint64_t(it->second)+86400<now) it=state_->routeRetry.erase(it);else ++it;
    for(auto it=state_->inviteRetry.begin();it!=state_->inviteRetry.end();)
        if(uint64_t(it->second)+86400<now) it=state_->inviteRetry.erase(it);else ++it;
}
