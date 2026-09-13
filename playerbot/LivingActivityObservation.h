#pragma once
#include "LivingActivity.h"
#include <algorithm>

namespace LivingActivity {
    // An unreadable checkpoint still stays quarantined. Only a never accepted,
    // resource-free observation may be proved irrelevant to new procurement.
    // Read alongside its envelope in the SAME bounded startup query; a missing
    // proof defaults to blocking. No special actor or fixture ID is exempted.
    inline std::string ResourceFreeObservationPredicate() {
        const std::string t="living_activity_task.";
        return t+"mode='observe' AND "+t+"accepted=0 AND "+t+"owner_generation=0 AND "+
            t+"parent_task_id='' AND "+t+"root_task_id="+t+"task_id AND "+
            "NOT EXISTS(SELECT 1 FROM living_activity_claim c WHERE c.task_id="+t+"task_id) AND "+
            "NOT EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.task_id="+t+"task_id) AND "+
            "NOT EXISTS(SELECT 1 FROM living_activity_task child WHERE child.parent_task_id="+t+"task_id OR "+
            "(child.root_task_id="+t+"task_id AND child.task_id<>"+t+"task_id))";
    }
    // Only the legacy planner's NEVER accepted, never executed projections.
    // Accepted jobs and native source rows are deliberately outside this scope.
    inline bool IsRetirableEconomyObservation(const Task& task) {
        return task.source=="economy_goal" && task.mode==Mode::Observe && !task.accepted &&
            task.phase==Phase::Reconciling && task.root==task.id && task.parent.empty() &&
            !task.sourceKey.empty() && task.sourceKey.size()<=20 && task.sourceKey[0]!='0' &&
            task.sourceKey.find_first_not_of("0123456789")==std::string::npos;
    }
    inline std::string EconomyObservationRetirementPredicate() {
        // Used in both selection and the transaction's UPDATE. Native source
        // reactivation, a new claim or accepted work between them rejects it.
        const std::string t="living_activity_task.";
        return t+"source='economy_goal' AND "+t+"mode='observe' AND "+t+"accepted=0 AND "+
            t+"phase='reconciling' AND "+t+"parent_task_id='' AND "+
            t+"root_task_id="+t+"task_id AND "+t+"source_key REGEXP '^[1-9][0-9]{0,19}$' AND "
            "NOT EXISTS(SELECT 1 FROM organic_economy_goal g WHERE g.goal_id=CAST("+t+"source_key AS UNSIGNED) "
            "AND g.character_guid="+t+"actor_guid AND g.state='active') AND "
            "NOT EXISTS(SELECT 1 FROM living_activity_claim c WHERE c.task_id="+t+"task_id) AND "
            "NOT EXISTS(SELECT 1 FROM living_activity_operation o WHERE o.task_id="+t+"task_id) AND "
            "NOT EXISTS(SELECT 1 FROM living_activity_task child WHERE child.parent_task_id="+t+"task_id OR "
            "(child.root_task_id="+t+"task_id AND child.task_id<>"+t+"task_id))";
    }
    inline bool PrepareEconomyObservationRetirement(const Task& before,uint64_t now,Task& after) {
        after={};std::string error;
        if(!Validate(before,error) || !IsRetirableEconomyObservation(before) || !now || before.revision>=UINT64_MAX-1)
            return false;
        after=before;++after.revision;after.phase=Phase::Cancelled;
        after.updatedAtMs=std::max(now,before.updatedAtMs);after.retryAtMs=0;
        after.checkpoint.step="observer_retired";
        after.checkpoint.blocker="legacy_candidate_no_longer_active";
        return true;
    }
}
