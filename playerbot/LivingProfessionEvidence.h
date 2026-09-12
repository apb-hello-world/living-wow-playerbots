#ifndef LIVING_PROFESSION_EVIDENCE_H
#define LIVING_PROFESSION_EVIDENCE_H
#include "LivingCraftCapture.h"
#include <array>

namespace LivingActivity {
    // Immutable row returned by the authenticated character-database reader,
    // NOT a pending callback, a model proposal, or current inventory totals.
    // Loading the row does not reconstruct an execution lease/world context.
    struct StoredCraftOperation {
        OperationResult receipt;
        std::string beforeState, afterState;
        bool acknowledged=false; // Only a completed DB read may set this.
    };
    struct StoredCraftProof {
        ProfessionCraftProof attempt;
        std::vector<ClaimConsumption> inputs;
        std::vector<VerifiedItemGain> gains;
    };
    // Supports the exact saved create/transform and no-effect cancellation
    // formats. Unknown, partial or malformed outcomes stay unresolved. This
    // cannot execute, mark a task completed, release claims or grant items.
    bool DecodeStoredCraftProof(const Task& task, const StoredCraftOperation& row,
        StoredCraftProof& result, std::string& blocker);
    struct InterruptedCraftIntent {
        uint32_t skill=0,money=0;
        ItemGainSpec output;
        std::vector<ClaimConsumption> inputs;
    };
    // Only an acknowledged, still-intent atomic profession save is eligible.
    // This decodes identity, not proof that a native effect did or did not run.
    bool DecodeInterruptedCraftIntent(const Task&,const StoredCraftOperation&,
        InterruptedCraftIntent&,std::string& blocker);

    // Exact, bounded result of ProfessionHistoryQuery. Fields are actor, task
    // revision, unresolved-actor-operation flag, operation ID, operation task,
    // operation revision, kind, state, native reference, before, after, evidence.
    // An empty operation ID represents the LEFT JOIN's no-history row.
    using ProfessionHistoryRow=std::array<std::string,12>;
    struct ProfessionHistory {
        std::string task;
        uint64_t revision=0;
        bool complete=false, unresolvedOperation=true;
        std::vector<ProfessionCraftProof> attempts;
        std::optional<StoredCraftOperation> interruptedCraft;
        std::optional<StoredCraftOperation> interruptedMail;
    };
    std::string ProfessionHistoryQuery(const Task& task);
    // Value-only, finite decoder. Begin validates the read's identity/envelope;
    // Advance validates at most one saved craft per world update. No DB calls,
    // native pointers, new scheduler, or permission to repeat an operation.
    class ProfessionHistoryCursor {
    public:
        bool Begin(const Task& task, const std::vector<ProfessionHistoryRow>& rows, std::string& blocker);
        bool Advance(std::string& blocker);
        const ProfessionHistory& Result() const {return history;}
    private:
        Task task;
        ProfessionHistory history;
        std::vector<StoredCraftOperation> records;
        size_t position=0;
    };
}
#endif
