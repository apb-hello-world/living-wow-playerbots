#ifndef LIVING_PROFESSION_JOB_H
#define LIVING_PROFESSION_JOB_H
#include "LivingActivity.h"
#include <vector>

namespace LivingActivity {
    enum class ProfessionOperation { CreateItem, TransformMaterial, EnchantItem, DisenchantItem };
    enum class ProfessionPurpose { SkillGain, Intermediate, RequestedItem, Equipment };
    struct ProfessionReagent {
        uint32_t entry = 0, perAttempt = 0;
        bool operator==(const ProfessionReagent& other) const { return entry == other.entry && perAttempt == other.perAttempt; }
    };
    // Exact accepted intent lives in the existing task checkpoint. Inventory,
    // paid orders, consumed quantities and outcomes live in the shared claims
    // and operation journal, not a second profession ledger or timer.
    struct ProfessionJob {
        uint32_t recipe = 0, skill = 0, subjectItem = 0;
        uint32_t outputEntry = 0, outputQuantity = 0;
        uint32_t initialSkill = 0, targetSkill = 0, attemptLimit = 5;
        ProfessionOperation operation = ProfessionOperation::CreateItem;
        ProfessionPurpose purpose = ProfessionPurpose::SkillGain;
        std::vector<ProfessionReagent> reagents;
    };
    bool ValidateProfessionJob(const ProfessionJob& job, std::string& blocker);
    std::string EncodeProfessionJob(const ProfessionJob& job);
    bool DecodeProfessionJob(const std::string& data, ProfessionJob& job, std::string& blocker);
    bool IsProfessionJob(const Task& task);
    bool ValidateProfessionTask(const Task& task, std::string& blocker);
    bool PreserveProfessionIntent(const Task& before, const Task& after, std::string& blocker);

    struct ProfessionMaterialLink {
        std::string task, operation;
        uint32_t actor = 0, recipe = 0, entry = 0, quantity = 0;
        uint64_t nativeReference = 0;
    };
    // Checks identity only; the native order/receipt must independently verify.
    // A reagent appearing in a mailbox is never sufficient to infer its recipe.
    bool MatchesProfessionMaterial(const Task& task, const ProfessionMaterialLink& link);
}
#endif
