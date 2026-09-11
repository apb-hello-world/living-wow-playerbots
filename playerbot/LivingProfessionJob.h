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

    // Value-only native recipe facts. Produced synchronously from the native
    // spellbook, skill-line records and owned subject item; never from a model.
    struct NativeProfessionRecipe {
        uint32_t recipe = 0, skill = 0, skillValue = 0, skillMaximum = 0, greyAt = 0;
        uint32_t outputEntry = 0;
        ProfessionOperation operation = ProfessionOperation::CreateItem;
        std::vector<ProfessionReagent> reagents;
        bool known = false, subjectOwned = false;
        std::string blocker = "native_profession_recipe_not_inspected";
    };
    bool MatchNativeProfessionRecipe(const ProfessionJob& job, const NativeProfessionRecipe& native,
        std::string& blocker);

    struct ProfessionMaterialLink {
        std::string task, operation;
        uint32_t actor = 0, recipe = 0, entry = 0, quantity = 0;
        uint64_t nativeReference = 0;
    };
    // Checks identity only; the native order/receipt must independently verify.
    // A reagent appearing in a mailbox is never sufficient to infer its recipe.
    bool MatchesProfessionMaterial(const Task& task, const ProfessionMaterialLink& link);

    // Finite profession decisions, not another scheduler. A world-thread
    // adapter supplies a fresh native/acknowledged-journal snapshot; callers
    // still acquire the root's authority for every selected service step.
    enum class ProfessionStep {
        Reconcile, Pause, Defer, PrepareCapacity, Withdraw, Collect, Purchase,
        WaitForDelivery, PrepareTools, ReachStation, Execute, Finalize
    };
    struct ProfessionStock {
        uint32_t entry = 0;
        // Disjoint locations. Bag/bank counts exclude other jobs' protection.
        // Incoming counts require an exact paid-order link, not matching mail.
        uint32_t bag = 0, bank = 0, paidInTransit = 0, delivered = 0;
        bool sourceAvailable = false;
    };
    struct ProfessionCraftProof {
        OperationResult receipt;
        uint32_t recipe = 0, subjectItem = 0;
        std::vector<ProfessionReagent> consumed, produced; // perAttempt = actual quantity
        uint32_t skillBefore = 0, skillAfter = 0;
        bool committed = false, nativeEffectVerified = false;
    };
    struct ProfessionSnapshot {
        std::string task;
        uint64_t revision = 0;
        WorldContext context;
        bool complete = false, unresolvedOperation = true, safe = false;
        bool retryReady = false; // Existing due queue/backoff, not a new profession timer.
        bool knownRecipe = false, useful = false, capacity = false;
        bool bankAccess = false, tools = false, atStation = false;
        uint32_t skill = 0;
        std::vector<ProfessionStock> stock;
        std::vector<ProfessionCraftProof> attempts;
    };
    struct ProfessionDecision {
        ProfessionStep step = ProfessionStep::Reconcile;
        std::string blocker;
        std::vector<ProfessionReagent> quantities;
        uint32_t verifiedAttempts = 0;
        uint64_t verifiedOutput = 0;
    };
    ProfessionDecision NextProfessionStep(const Task& task, const ProfessionSnapshot& snapshot);
}
#endif
