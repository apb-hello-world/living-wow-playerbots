#ifndef LIVING_GUILD_EVENT_COMMITMENT_H
#define LIVING_GUILD_EVENT_COMMITMENT_H
#include "LivingActivity.h"
#include "GuildGovernancePolicy.h"
#include <boost/property_tree/json_parser.hpp>
#include <sstream>
#include <stdexcept>
namespace LivingActivity {
// One participant's immutable acceptance of one calendar revision. Calendar
// editing requires renewed acceptance/new source identity, not silent replanning
// of a committed route. Roster/coordinator changes remain native event state.
struct GuildEventCommitment {
    std::string event,kind;
    uint32_t guild=0,eventRevision=0,target=0,starts=0,ends=0;
};
inline bool ValidGuildEventCommitment(const GuildEventCommitment& c) {
    return livingguild::Id(c.event) && c.guild && c.eventRevision && c.target && c.starts && c.ends>c.starts &&
        (c.kind=="quest" || c.kind=="dungeon");
}
inline std::string GuildEventCommitmentKey(const GuildEventCommitment& c,uint32_t actor) {
    if(!ValidGuildEventCommitment(c) || !actor)throw std::invalid_argument("exact_guild_event_acceptance_required");
    return c.event+":"+std::to_string(c.eventRevision)+":"+std::to_string(actor);
}
inline std::string EncodeGuildEventCommitment(const GuildEventCommitment& c) {
    if(!ValidGuildEventCommitment(c))throw std::invalid_argument("invalid_guild_event_commitment");
    return "{\"workflow\":\"guild_event_v1\",\"event\":\""+c.event+"\",\"kind\":\""+c.kind+
        "\",\"guild\":"+std::to_string(c.guild)+",\"event_revision\":"+std::to_string(c.eventRevision)+
        ",\"target\":"+std::to_string(c.target)+",\"starts\":"+std::to_string(c.starts)+",\"ends\":"+std::to_string(c.ends)+'}';
}
inline bool DecodeGuildEventCommitment(const std::string& data,GuildEventCommitment& out) {
    out={};if(data.empty() || data.size()>512)return false;
    try {
        boost::property_tree::ptree p;std::istringstream stream(data);boost::property_tree::read_json(stream,p);
        if(p.get<std::string>("workflow")!="guild_event_v1")return false;
        GuildEventCommitment c;c.event=p.get<std::string>("event");c.kind=p.get<std::string>("kind");
        c.guild=p.get<uint32_t>("guild");c.eventRevision=p.get<uint32_t>("event_revision");c.target=p.get<uint32_t>("target");
        c.starts=p.get<uint32_t>("starts");c.ends=p.get<uint32_t>("ends");
        // Canonical private checkpoints reject duplicate/unknown fields,
        // wrapped numbers and ambiguous strings instead of normalizing them.
        if(!ValidGuildEventCommitment(c) || EncodeGuildEventCommitment(c)!=data)return false;
        out=std::move(c);return true;
    } catch(const std::exception&) {return false;}
}
inline bool IsGuildEventCommitment(const Task& task) {
    return task.mode==Mode::Active && (task.kind==Kind::GuildEvent || task.source=="guild_event_commitment");
}
inline bool ValidateGuildEventCommitmentTask(const Task& task,std::string& why) {
    if(!IsGuildEventCommitment(task)){why.clear();return true;}
    GuildEventCommitment c;
    bool step=false;
    for(const auto* value:{"guild_event_wait","guild_event_form","guild_event_prepare","guild_event_travel",
        "guild_event_objective","guild_event_return","guild_event_verify","guild_event_closed","guild_event_group",
        "guild_quest_reward_ready","guild_quest_reward"})step|=task.checkpoint.step==value;
    if(task.source!="guild_event_commitment" || task.kind!=Kind::GuildEvent || task.root!=task.id ||
        !task.parent.empty() || !task.accepted || !task.actor || task.priority!=Priority::Scheduled || !step ||
        !DecodeGuildEventCommitment(task.checkpoint.data,c) || task.sourceKey!=GuildEventCommitmentKey(c,task.actor) ||
        task.dueAtMs!=uint64_t(c.starts)*1000) {
        why="invalid_guild_event_commitment";return false;
    }
    why.clear();return true;
}
inline bool PreserveGuildEventCommitment(const Task& before,const Task& after,std::string& why) {
    if(!ValidateGuildEventCommitmentTask(after,why))return false;
    if(IsGuildEventCommitment(before) && before.accepted &&
        (!IsGuildEventCommitment(after) || before.checkpoint.data!=after.checkpoint.data || before.dueAtMs!=after.dueAtMs)) {
        why="accepted_guild_event_revision_is_immutable";return false;
    }
    why.clear();return true;
}
// Value-only native calendar/RSVP projection. This is eligibility, NOT a lease
// or completion proof. Producers must re-read native membership/delegation and
// event revision; no model, title, timer or reconstructed roster is sufficient.
struct GuildEventAcceptance {
    GuildEventCommitment definition;
    uint32_t actor=0,memberGuild=0,acceptedRevision=0;
    bool accepted=false,delegated=false,terminal=false;
};
inline const char* GuildEventAcceptanceBlocker(const GuildEventCommitment& saved,uint32_t actor,const GuildEventAcceptance& current) {
    if(!ValidGuildEventCommitment(saved) || !ValidGuildEventCommitment(current.definition) || !actor || actor!=current.actor)
        return "guild_event_native_record_unavailable";
    if(current.memberGuild!=saved.guild || current.definition.guild!=saved.guild)return "guild_event_membership_changed";
    if(!current.delegated)return "guild_event_delegation_revoked";
    if(current.definition.event!=saved.event || current.definition.eventRevision!=saved.eventRevision ||
        EncodeGuildEventCommitment(saved)!=EncodeGuildEventCommitment(current.definition))return "guild_event_revision_requires_renewal";
    if(!current.accepted || current.acceptedRevision!=saved.eventRevision)return "guild_event_acceptance_withdrawn";
    return current.terminal?"guild_event_terminal_requires_reconciliation":"";
}
}
#endif
