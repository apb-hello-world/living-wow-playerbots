#pragma once
#include "LivingGuildEventCommitment.h"
#include "LivingActivityJournal.h"

namespace LivingActivity {
// Terminal calendar projection, not a caller's success boolean. The journal
// repeats all predicates against native records before acknowledging closure.
struct GuildEventClosure {
    std::string state,reason;
    uint32_t started=0,finished=0,participantVerified=0,instance=0;
};
struct GuildEventSettlement {Task task;WritePlan plan;};
// Always returns a row on a successful read. Missing/revised authority is an
// explicit cancellation fact, never confused with a failed database query.
inline std::string GuildEventInvalidationExpression(const Task& task,const GuildEventCommitment& c) {
    const auto event=SqlValue(c.event),guild=std::to_string(c.guild),actor=std::to_string(task.actor);
    return "CASE WHEN NOT EXISTS(SELECT 1 FROM guild_society_event changed WHERE changed.event_id="+event+
        ") THEN 'guild_event_removed' WHEN EXISTS(SELECT 1 FROM guild_society_event changed WHERE changed.event_id="+event+
        " AND (changed.guild_id<>"+guild+" OR changed.revision<>"+std::to_string(c.eventRevision)+
        " OR changed.event_type<>"+SqlValue(c.kind)+" OR changed.target_id<>"+std::to_string(c.target)+
        " OR changed.scheduled_at<>"+std::to_string(c.starts)+" OR COALESCE(changed.ends_at,changed.scheduled_at+3600)<>"+
        std::to_string(c.ends)+")) THEN 'guild_event_revision_requires_renewal'"
        " WHEN NOT EXISTS(SELECT 1 FROM guild_member member WHERE member.guid="+actor+" AND member.guildid="+guild+
        ") THEN 'guild_event_membership_changed' WHEN NOT EXISTS(SELECT 1 FROM guild_society_rsvp accepted WHERE accepted.event_id="+event+
        " AND accepted.character_guid="+actor+" AND accepted.response='accepted' AND accepted.accepted_revision="+
        std::to_string(c.eventRevision)+") THEN 'guild_event_acceptance_withdrawn' ELSE '' END";
}
inline std::string GuildEventClosureQuery(const Task& task,const GuildEventCommitment& c) {
    return "SELECT e.state,e.failure_reason,e.started_at,e.finished_at,COALESCE(p.verified_at,0),e.activity_instance_id,"+
        GuildEventInvalidationExpression(task,c)+
        " FROM (SELECT 1) probe LEFT JOIN guild_society_event e ON e.event_id="+SqlValue(c.event)+" AND e.guild_id="+std::to_string(c.guild)+
        " AND e.revision="+std::to_string(c.eventRevision)+" AND e.event_type="+SqlValue(c.kind)+
        " AND e.target_id="+std::to_string(c.target)+" AND e.scheduled_at="+std::to_string(c.starts)+
        " AND COALESCE(e.ends_at,e.scheduled_at+3600)="+std::to_string(c.ends)+
        " LEFT JOIN guild_society_event_participant p ON p.event_id=e.event_id"
        " AND p.revision=e.revision AND p.character_guid="+std::to_string(task.actor);
}
inline bool PrepareGuildEventInvalidation(const Task& saved,const WorldContext& current,
    const std::string& reason,uint64_t now,const std::string& receipt,GuildEventSettlement& out,std::string& why) {
    out={};GuildEventCommitment c;
    if(!Validate(saved,why) || !IsGuildEventCommitment(saved) || !ValidateGuildEventCommitmentTask(saved,why) ||
        !DecodeGuildEventCommitment(saved.checkpoint.data,c) || Terminal(saved.phase) ||
        current.actor!=saved.actor || !current.actorGeneration || !current.mapGeneration || !current.policyRevision ||
        !IsUuid(current.boot) || !IsUuid(receipt) || now<saved.updatedAtMs || saved.revision>=UINT64_MAX-1) {
        why="guild_event_invalidation_context_invalid";return false;
    }
    if(reason!="guild_event_removed" && reason!="guild_event_revision_requires_renewal" &&
        reason!="guild_event_membership_changed" && reason!="guild_event_acceptance_withdrawn") {
        why="guild_event_invalidation_evidence_required";return false;
    }
    out.task=saved;auto& next=out.task;++next.revision;next.context=current;next.updatedAtMs=now;next.retryAtMs=0;
    next.phase=Phase::Cancelled;next.checkpoint.step="guild_event_closed";next.checkpoint.blocker=reason;
    out.plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,"guild_event_native_acceptance_invalidated",reason);
    out.plan.statements.front()+=" AND checkpoint="+SqlValue(saved.checkpoint.data)+
        " AND ("+GuildEventInvalidationExpression(saved,c)+")="+SqlValue(reason)+
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
        " WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id"
        " AND c.state NOT IN ('consumed','released'))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_task child WHERE child.root_task_id=living_activity_task.task_id"
        " AND child.task_id<>living_activity_task.task_id AND child.phase NOT IN ('completed','cancelled','failed'))";
    why.clear();return true;
}
inline bool PrepareGuildEventSettlement(const Task& saved,const WorldContext& current,
    const GuildEventClosure& native,uint64_t now,const std::string& receipt,GuildEventSettlement& out,std::string& why) {
    out={};GuildEventCommitment c;auto reject=[&](const char* code){why=code;return false;};
    if(!Validate(saved,why) || !IsGuildEventCommitment(saved) || !ValidateGuildEventCommitmentTask(saved,why) ||
        !DecodeGuildEventCommitment(saved.checkpoint.data,c) || Terminal(saved.phase) ||
        !(saved.context==current) || current.actor!=saved.actor || !current.actorGeneration || !current.mapGeneration ||
        !IsUuid(current.boot) || !IsUuid(receipt) || now<saved.updatedAtMs || saved.revision>=UINT64_MAX-1)
        return reject("guild_event_settlement_context_invalid");
    const bool complete=native.state=="completed";
    if(!complete && native.state!="failed" && native.state!="cancelled")return reject("guild_event_not_terminal");
    if(!native.finished || uint64_t(native.finished)*1000>now || native.reason.empty() || !IsToken(native.reason))
        return reject("guild_event_terminal_reason_required");
    if(complete && (native.started<c.starts || native.started>native.participantVerified ||
        native.participantVerified>native.finished || native.participantVerified>=c.ends ||
        native.reason!=(c.kind=="quest"?"quest_reward_verified":"dungeon_encounters_verified") ||
        (c.kind=="dungeon" && !native.instance)))return reject("guild_event_participant_outcome_not_verified");
    out.task=saved;auto& next=out.task;++next.revision;next.updatedAtMs=now;next.retryAtMs=0;
    next.phase=complete?Phase::Completed:native.state=="failed"?Phase::Failed:Phase::Cancelled;
    next.checkpoint.step="guild_event_closed";next.checkpoint.blocker=native.reason;
    const auto n=[](uint64_t value){return std::to_string(value);};
    const std::string code=complete?"guild_event_native_outcome_verified":"guild_event_native_terminal_reconciled";
    out.plan=Detail::TaskTransitionWrite(next,saved.revision,receipt,code,native.state+'|'+native.reason+'|'+
        n(native.started)+'|'+n(native.finished)+'|'+n(native.participantVerified)+'|'+n(native.instance));
    auto& guard=out.plan.statements.front();
    guard+=" AND checkpoint="+SqlValue(saved.checkpoint.data)+
        " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
        " WHERE t.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id"
        " AND c.state NOT IN ('consumed','released'))"
        " AND NOT EXISTS(SELECT 1 FROM living_activity_task child WHERE child.root_task_id=living_activity_task.task_id"
        " AND child.task_id<>living_activity_task.task_id AND child.phase NOT IN ('completed','cancelled','failed'))";
    guard+=" AND EXISTS(SELECT 1 FROM guild_society_event e WHERE e.event_id="+SqlValue(c.event)+
        " AND e.guild_id="+n(c.guild)+" AND e.revision="+n(c.eventRevision)+" AND e.event_type="+SqlValue(c.kind)+
        " AND e.target_id="+n(c.target)+" AND e.scheduled_at="+n(c.starts)+
        " AND COALESCE(e.ends_at,e.scheduled_at+3600)="+n(c.ends)+" AND e.state="+SqlValue(native.state)+
        " AND e.failure_reason="+SqlValue(native.reason)+" AND e.started_at="+n(native.started)+
        " AND e.finished_at="+n(native.finished)+" AND e.activity_instance_id="+n(native.instance);
    if(complete) {
        guard+=" AND EXISTS(SELECT 1 FROM guild_society_event_participant p WHERE p.event_id=e.event_id"
            " AND p.revision=e.revision AND p.character_guid="+n(saved.actor)+" AND p.verified_at="+n(native.participantVerified);
        if(c.kind=="quest")guard+=" AND p.began_incomplete=1";
        guard+=") AND EXISTS(SELECT 1 FROM guild_society_activity_proof p WHERE p.event_id=e.event_id"
            " AND p.revision=e.revision AND p.character_guid="+n(saved.actor)+
            " AND p.occurred_at>=e.started_at AND p.occurred_at<=e.finished_at AND p.occurred_at<COALESCE(e.ends_at,e.scheduled_at+3600)";
        guard+=c.kind=="quest"?" AND p.kind=2 AND p.entry="+n(c.target):
            " AND p.kind=1 AND p.map_id="+n(c.target)+" AND p.instance_id="+n(native.instance);
        guard+=") AND EXISTS(SELECT 1 FROM guild_society_credit credit WHERE credit.guild_id=e.guild_id"
            " AND credit.character_guid="+n(saved.actor)+" AND credit.source_id=CONCAT('event:',e.event_id)"
            " AND credit.source_type="+SqlValue(c.kind=="quest"?"verified_quest":"verified_dungeon")+
            " AND credit.earned_at="+n(native.participantVerified)+')';
    }
    guard+=')';why.clear();return true;
}
}
