#pragma once
#include <cstdint>
#include <string>
namespace LivingActivity {
enum class EconomyOwnershipProjection { Pending, LegacyOnly, Managed };
// Mode is deliberately absent: disabling execution does not erase ownership.
// A failed/partial schema check is not evidence that no saved owner exists.
inline EconomyOwnershipProjection EconomyOwnershipState(bool inspected,bool absent,bool verified) {
    if (verified) return EconomyOwnershipProjection::Managed;
    if (inspected && absent) return EconomyOwnershipProjection::LegacyOnly;
    return EconomyOwnershipProjection::Pending;
}
// Shared by the real legacy projection and MariaDB integration tests. A saved
// accepted owner survives candidate expiry; no recipe-label lookup can replace it.
inline std::string EconomyProfessionProfilesQuery(bool managed) {
    const std::string join=managed ?
        " LEFT JOIN living_activity_task owned ON owned.source='profession_job' AND owned.source_key=CONCAT('economy_goal:',candidate.goal_id) AND owned.actor_guid=candidate.character_guid AND owned.mode='active' AND owned.accepted=1 " : " ";
    const std::string projection=managed ? "COALESCE(managed.task_id,''),COALESCE(managed.phase,'') " : "'','' ";
    return std::string("SELECT profile.character_guid,profile.career_participant,COALESCE(profile.intended_profession_one,0),")+
        "COALESCE(profile.intended_profession_two,0),COALESCE(goal.capability_ref,''),"
        "COALESCE(goal.goal_type,''),COALESCE(goal.state,''),profile.profession_plan_version,actor.race,"
        "COALESCE(UNIX_TIMESTAMP(goal.created_at),0),COALESCE(JSON_EXTRACT(goal.authoritative_payload,'$.paid_materials_until'),0),COALESCE(goal.goal_id,0),"+projection+
        "FROM organic_economy_profile profile JOIN characters actor ON actor.guid=profile.character_guid "
        "LEFT JOIN organic_economy_goal goal ON goal.goal_id=(SELECT candidate.goal_id FROM organic_economy_goal candidate "+join+
        "WHERE candidate.character_guid=profile.character_guid AND candidate.state IN ('active','proposed','candidate') "
        "AND (candidate.expires_at IS NULL OR candidate.expires_at>NOW()"+(managed ? std::string(" OR owned.task_id IS NOT NULL") : "")+
        ") ORDER BY "+(managed ? std::string("(owned.task_id IS NOT NULL) DESC,") : "")+"candidate.goal_id DESC LIMIT 1) "+
        (managed ? "LEFT JOIN living_activity_task managed ON managed.source='profession_job' AND managed.source_key=CONCAT('economy_goal:',goal.goal_id) AND managed.actor_guid=goal.character_guid AND managed.mode='active' AND managed.accepted=1" : "");
}
inline std::string EconomyProfessionExpiryQuery(uint32_t actor,bool managed) {
    return "UPDATE organic_economy_goal SET state='expired' WHERE character_guid="+std::to_string(actor)+
        " AND state IN ('candidate','proposed','active')"+(managed ?
        " AND NOT EXISTS (SELECT 1 FROM living_activity_task owned WHERE owned.source='profession_job' AND owned.source_key=CONCAT('economy_goal:',organic_economy_goal.goal_id) AND owned.actor_guid=organic_economy_goal.character_guid AND owned.mode='active' AND owned.accepted=1)" : "");
}
}
