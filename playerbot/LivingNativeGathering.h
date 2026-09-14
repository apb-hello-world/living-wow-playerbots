#pragma once
#include "LivingNativeCraftCapture.h"
#include "LivingGatherQuote.h"
#include "LivingLootQuote.h"
class GameObject;
class PlayerbotAI;
namespace LivingActivity {
    // Cached native loot-table sources, filtered by known skill and supported
    // mechanics. No database scan, generated item or inferred loot quantity.
    bool NativeGatherSources(Player&,uint32_t entry,std::vector<int32_t>& sources,uint32_t& purpose,std::string&);
    GameObject* NativeGatherNode(Player&,uint32_t entry,const std::vector<int32_t>& sources);
    bool InspectNativeGatherQuote(Player&,uint64_t source,uint32_t entry,NativeGatherQuote&,std::string&);
    bool FindNativeRequestedLoot(Player&,uint32_t entry,NativeLootQuote&,std::string&);
    bool HoldsManagedGatherLoot(PlayerbotAI&,uint64_t source);
    class NativeGatherOperation final : public NativeOperationAdapter {
    public:
        const char* OperationKind() const override {return "gather_open";}
        uint32_t OperationEffects() const override;
        NativePersistence PersistencePolicy() const override {return NativePersistence::Profession;}
        bool DeferredNativeCast() const override {return true;}
        bool ValidateNative(Player&,const OperationRequest&,std::string&) override;
        std::shared_ptr<NativeCraftCast> ReserveNativeCast(const OperationRequest&,const Task&,const ActionContext&) const override;
        NativeObservation ExecuteNative(Player&,const OperationRequest&) override {
            return {OperationState::Rejected,"","gather_requires_native_cast","{}"};
        }
    };
}
