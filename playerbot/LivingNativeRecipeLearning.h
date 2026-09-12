#pragma once
#include "LivingNativeCraftCapture.h"
#include "LivingRecipeLearning.h"
#include "LivingActivityReservations.h"

namespace LivingActivity {
    bool BuildNativeRecipeLearningJob(Player& actor,uint32_t book,RecipeLearningJob& job,std::string& blocker);
    bool ValidateNativeRecipeLearningTask(Player& actor,const Task& task,std::string& blocker);
    class NativeRecipeBookReservation final : public NativeReservationAdapter {
    public:
        bool ValidatePurpose(Player&,const ReservationRequest&,std::string&) override;
    };
    class NativeRecipeLearningOperation final : public NativeOperationAdapter {
    public:
        const char* OperationKind() const override {return "recipe_learning";}
        uint32_t OperationEffects() const override;
        bool SupportsClaimedConsumption() const override {return true;}
        NativePersistence PersistencePolicy() const override {return NativePersistence::Profession;}
        bool DeferredNativeCast() const override {return true;}
        bool ValidateNative(Player&,const OperationRequest&,std::string&) override;
        std::shared_ptr<NativeCraftCast> ReserveNativeCast(const OperationRequest&,const Task&,const ActionContext&) const override;
        NativeObservation ExecuteNative(Player&,const OperationRequest&) override {
            return {OperationState::Rejected,"","recipe_learning_requires_deferred_dispatch","{}"};
        }
    };
}
