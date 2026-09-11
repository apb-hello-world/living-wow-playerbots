#pragma once
#include "LivingCraftCapture.h"
#include "LivingActivityClaimConsumption.h"
#include <set>

namespace LivingActivity {
    struct ProfessionAttemptPlan {
        std::vector<ClaimConsumption> consumption;
        ItemGainSpec output;
        std::string beforeState;
    };
    // Finite preparation from current native facts and acknowledged claims.
    // No test IDs, new reservation, purchase, spell, timer or persistence here.
    inline bool PlanProfessionAttempt(const Task& task,const ProfessionSnapshot& snapshot,
        const UnsettledClaimBatch& batch,const CraftFrame& frame,ProfessionAttemptPlan& plan,std::string& blocker) {
        plan={};
        auto reject=[&](const char* code){blocker=code;return false;};
        if (task.mode!=Mode::Active || task.root!=task.id || !task.parent.empty() ||
            (task.phase!=Phase::Preparing && task.phase!=Phase::Traveling))
            return reject("profession_attempt_requires_saved_preparation");
        const auto next=NextProfessionStep(task,snapshot);
        if (next.step!=ProfessionStep::Execute) {blocker=next.blocker;return false;}
        ProfessionJob job;
        if (!DecodeProfessionJob(task.checkpoint.data,job,blocker)) return false;
        if ((job.operation!=ProfessionOperation::CreateItem && job.operation!=ProfessionOperation::TransformMaterial) || job.subjectItem)
            return reject("profession_attempt_operation_not_supported");
        if (!batch.complete || batch.claims.size()>16 || !ValidCraftFrame(frame) || frame.actor!=task.actor ||
            frame.skill!=snapshot.skill || !snapshot.outputPerAttempt)
            return reject("profession_attempt_native_snapshot_incomplete");
        std::set<std::string> ids;
        for (const auto& claim : batch.claims)
            if (!ValidResourceClaim(claim) || claim.task!=task.id || claim.actor!=task.actor || !ids.insert(claim.id).second)
                return reject("profession_attempt_claim_identity_mismatch");
        ProfessionAttemptPlan result;
        for (const auto& reagent : job.reagents) {
            const NativeItemStack* stack=nullptr;
            for (const auto& candidate : frame.stacks) if (candidate.entry==reagent.entry) {
                if (stack) return reject("profession_attempt_mixed_stack_preparation_required");
                stack=&candidate;
            }
            if (!stack || stack->actor!=task.actor || stack->count<reagent.perAttempt)
                return reject("profession_attempt_native_material_missing");
            const ResourceClaim* owned=nullptr;
            for (const auto& claim : batch.claims) if (claim.itemEntry==reagent.entry) {
                if (owned || claim.itemGuid!=stack->guid || claim.state!="held" || claim.location!="bags" ||
                    claim.copper || claim.nativeReference || claim.quantity<reagent.perAttempt || claim.quantity>stack->count)
                    return reject("profession_attempt_exact_material_claim_required");
                owned=&claim;
            }
            if (!owned) return reject("profession_attempt_material_reservation_required");
            result.consumption.push_back({*owned,reagent.perAttempt});
        }
        result.output={job.outputEntry,snapshot.outputPerAttempt};
        if (!ValidItemGainSpec(result.output)) return reject("profession_attempt_native_output_invalid");
        result.beforeState="{\"recipe\":"+std::to_string(job.recipe)+",\"skill\":"+std::to_string(frame.skill)+
            ",\"money\":"+std::to_string(frame.money)+'}';
        plan=std::move(result);blocker.clear();return true;
    }
}
