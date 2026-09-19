#include "botpch.h"
#include "LivingNativeGuildEvent.h"
#include "LivingActivityCoordinator.h"
#include "PlayerbotGuildGovernance.h"
#include "Guilds/GuildMgr.h"
namespace LivingActivity {
bool ReadNativeGuildEventAcceptance(Player& actor,const GuildEventCommitment& expected,GuildEventAcceptance& result,std::string& why) {
    result={};why="guild_event_native_record_unavailable";
    if(!sLivingActivityCoordinator.OnWorldThread() || !actor.GetPlayerbotAI() || actor.isRealPlayer() ||
        !ValidGuildEventCommitment(expected))return false;
    auto* guild=sGuildMgr.GetGuildById(actor.GetGuildId());
    if(!guild || actor.GetGuildId()!=expected.guild){why="guild_event_membership_changed";return false;}
    const auto sql="SELECT e.event_id,e.guild_id,e.event_type,e.revision,e.target_id,e.scheduled_at,"
        "COALESCE(e.ends_at,e.scheduled_at+3600),e.state,COALESCE(r.response,''),COALESCE(r.accepted_revision,0),"
        "COALESCE(m.guildid,0) FROM guild_society_event e LEFT JOIN guild_society_rsvp r ON r.event_id=e.event_id"
        " AND r.character_guid="+std::to_string(actor.GetGUIDLow())+
        " LEFT JOIN guild_member m ON m.guid="+std::to_string(actor.GetGUIDLow())+
        " WHERE e.event_id="+SqlValue(expected.event)+" LIMIT 1";
    const auto rows=CharacterDatabase.Query(sql.c_str());
    if(!rows || rows->GetFieldCount()!=11)return false;
    const auto* f=rows->Fetch();auto& c=result.definition;
    c.event=f[0].GetCppString();c.guild=f[1].GetUInt32();c.kind=f[2].GetCppString();c.eventRevision=f[3].GetUInt32();
    c.target=f[4].GetUInt32();c.starts=f[5].GetUInt32();c.ends=f[6].GetUInt32();
    const auto phase=f[7].GetCppString();
    result.actor=actor.GetGUIDLow();result.memberGuild=f[10].GetUInt32();
    result.accepted=f[8].GetCppString()=="accepted";result.acceptedRevision=f[9].GetUInt32();
    result.delegated=sGuildGovernance.IsAvailable() && sGuildGovernance.Allows(guild,"events");
    result.terminal=phase=="completed" || phase=="cancelled" || phase=="failed" || phase=="expired";
    if(!result.terminal && phase!="announced" && phase!="forming" && phase!="preparing" &&
        phase!="traveling" && phase!="active" && phase!="returning" && phase!="completing") {
        why="guild_event_not_executable";return false;
    }
    if(!ValidGuildEventCommitment(c))return false;
    why.clear();return true;
}
bool ValidateNativeGuildEventTask(Player& actor,const Task& task,std::string& why) {
    if(!IsGuildEventCommitment(task)){why.clear();return true;}
    GuildEventCommitment saved;GuildEventAcceptance current;
    if(actor.GetGUIDLow()!=task.actor || !ValidateGuildEventCommitmentTask(task,why) ||
        !DecodeGuildEventCommitment(task.checkpoint.data,saved)) {
        why="invalid_guild_event_commitment";return false;
    }
    if(!ReadNativeGuildEventAcceptance(actor,saved,current,why))return false;
    why=GuildEventAcceptanceBlocker(saved,task.actor,current);return why.empty();
}
}
