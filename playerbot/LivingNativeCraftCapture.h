#ifndef LIVING_NATIVE_CRAFT_CAPTURE_H
#define LIVING_NATIVE_CRAFT_CAPTURE_H
#include "LivingCraftCapture.h"
#include "LivingActivityScope.h"
#include "LivingActivityOperations.h"
#include <atomic>
#include <memory>
class Player;
class Spell;

namespace LivingActivity {
    bool ReadNativeCraftFrame(Player& actor,const ProfessionJob& job,CraftFrame& frame,std::string& blocker);
    // One reserved, value-only binding for one native Spell. The coordinator
    // retains the same capture until its guarded save is acknowledged. No
    // Player/Item/Spell pointer is retained here or delivered to another thread.
    class NativeCraftCast : public std::enable_shared_from_this<NativeCraftCast> {
    public:
        NativeCraftCast(Task executing,ActionContext action,ProfessionJob job,
            std::vector<ClaimConsumption> consumption,ItemGainSpec output);
        // World-thread launch only, BEFORE SpellStart. This does not start a
        // spell, manufacture a lease, waive native CheckCast, or prove success.
        bool Attach(Spell& spell,std::string& blocker);
        // Launches one ordinary, non-triggered native cast. False means no
        // native cast was started; a true return is NOT a completion result.
        bool Start(Player& actor,std::string& blocker);
        NativeObservation Observe(Player& actor,const CraftCaptureResult& result,
            std::vector<VerifiedItemGain>& gains) const;
        std::string PersistedProof(Player& actor,const Task& outcome) const;
        // Called by the native core immediately before its resource effects.
        // The returned attribution scope exists only for that synchronous call.
        std::unique_ptr<ExecutionScope> EnterEffect(Spell& spell);
        void Created(Spell& spell,uint32_t entry,uint32_t quantity) noexcept;
        void Finished(Spell& spell,bool succeeded) noexcept;
        void Abandon() noexcept;
        std::shared_ptr<CraftCapture> Capture() const { return capture; }
    private:
        Player* Actor(Spell& spell) const; // Synchronous resolver, never stored.
        bool InputsAllowed(Player& actor,const CraftFrame& frame,std::string& blocker) const;
        bool AuthorityAllowed(Player& actor) const;
        const Task task;
        const ActionContext action;
        const ProfessionJob job;
        const std::vector<ClaimConsumption> consumption;
        const ItemGainSpec output;
        const CraftIdentity identity;
        const std::shared_ptr<CraftCapture> capture;
        std::atomic<bool> attached{false};
    };
    // Compiled exact-output profession adapter; no scheduler, grants, or free
    // respec/trainer semantics. Native SpellStart still checks tools and station.
    class NativeCraftOperation final : public NativeOperationAdapter {
    public:
        const char* OperationKind() const override { return "profession_craft"; }
        uint32_t OperationEffects() const override;
        bool SupportsClaimedConsumption() const override { return true; }
        bool SupportsItemGain() const override { return true; }
        NativePersistence PersistencePolicy() const override { return NativePersistence::Profession; }
        bool DeferredNativeCast() const override { return true; }
        bool ValidateNative(Player& actor,const OperationRequest& request,std::string& blocker) override;
        std::shared_ptr<NativeCraftCast> ReserveNativeCast(const OperationRequest& request,
            const Task& executing,const ActionContext& action) const override;
        NativeObservation ExecuteNative(Player&,const OperationRequest&) override {
            return {OperationState::Rejected,"","native_craft_requires_deferred_dispatch","{}"};
        }
    };
    class NativeCraftFinishGuard {
    public:
        NativeCraftFinishGuard(Spell& spell,bool succeeded) noexcept : spell(spell),succeeded(succeeded) {}
        ~NativeCraftFinishGuard() noexcept;
    private:
        Spell& spell; // Native synchronous stack lifetime only.
        bool succeeded;
    };
}
#endif
