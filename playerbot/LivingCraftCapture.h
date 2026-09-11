#ifndef LIVING_CRAFT_CAPTURE_H
#define LIVING_CRAFT_CAPTURE_H
#include "LivingActivityItemGain.h"
#include "LivingProfessionJob.h"
#include <mutex>
#include <optional>

namespace LivingActivity {
    struct CraftIdentity {
        std::string task, operation;
        uint64_t revision=0, ownerGeneration=0;
        WorldContext world;
        uint32_t recipe=0, skill=0;
    };
    struct CraftFrame {
        uint32_t actor=0, skill=0, money=0;
        // Only the recipe's inputs and output; exact native bag identities.
        // The compiled collector validates ownership before publishing values.
        std::vector<NativeItemStack> stacks;
    };
    enum class CraftCapturePhase { Reserved, Casting, Applying, Finished, Abandoned };
    struct CraftCaptureResult {
        CraftIdentity identity;
        CraftCapturePhase phase=CraftCapturePhase::Reserved;
        CraftFrame before, after;
        bool effectEntered=false, nativeFinished=false, nativeSucceeded=false;
        uint32_t createdEntry=0, createdQuantity=0, createdCalls=0;
        std::string blocker;
    };
    // One slot owned by an already admitted operation, shared with exactly ONE
    // native Spell. No native pointers, task lookup, database operation, timer,
    // or gameplay permission. The coordinator's existing operation cap bounds
    // slots; it must reserve this slot BEFORE starting a nonrepeatable cast.
    // Native callbacks copy values synchronously; only the world journals them.
    class CraftCapture {
    public:
        explicit CraftCapture(CraftIdentity identity);
        bool Start(const CraftIdentity& identity, CraftFrame before);
        bool EnterEffect(const CraftIdentity& identity, const CraftFrame& current);
        bool Created(const CraftIdentity& identity, uint32_t entry, uint32_t quantity);
        bool Finish(const CraftIdentity& identity, bool succeeded, CraftFrame after);
        void Abandon(); // Missing native completion is uncertainty, never success.
        std::optional<CraftCaptureResult> ReadFinished() const;
        CraftCapturePhase Phase() const;
    private:
        mutable std::mutex mutex;
        CraftCaptureResult result;
    };
    bool SameCraftIdentity(const CraftIdentity& a, const CraftIdentity& b);
    bool ValidCraftFrame(const CraftFrame& frame);
    enum class CraftEvidence { Verified, RejectedWithoutEffect, Reconciling };
    struct CraftVerification {
        CraftEvidence result=CraftEvidence::Reconciling;
        std::string blocker="native_craft_not_verified";
        std::vector<VerifiedItemGain> gains;
        // A native callback is NOT a committed receipt. The save/journal path
        // fills receipt and committed only after observing its actual SQL proof.
        ProfessionCraftProof attempt;
    };
    // First finite adapter supports exact-output create/transform operations.
    // Variable outputs, enchant and disenchant require their own native proof;
    // do not interpret them as one create-item spell or silently discard extras.
    CraftVerification VerifyCraftCapture(const CraftIdentity& expected,
        const ProfessionJob& job, const CraftCaptureResult& observed, const ItemGainSpec& output);
}
#endif
