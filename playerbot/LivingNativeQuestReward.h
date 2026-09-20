#pragma once
#include "LivingQuestReward.h"
#include "LivingActivityOperations.h"
namespace LivingActivity {
bool PlanNativeQuestReward(Player&,const Task&,QuestRewardQuote&,std::string&);
NativeObservation InspectNativeQuestReward(Player&,const Task&,const QuestRewardQuote&);
class NativeQuestReward final : public NativeOperationAdapter {
public:
    explicit NativeQuestReward(QuestRewardQuote value):quote(std::move(value)) {}
    const char* OperationKind() const override {return "guild_quest_reward";}
    uint32_t OperationEffects() const override {return Mask(Effect::Inventory)|Mask(Effect::Money)|Mask(Effect::Spell);}
    NativePersistence PersistencePolicy() const override {return NativePersistence::Character;}
    bool SupportsQuestReward() const override {return true;}
    bool ValidateNative(Player&,const OperationRequest&,std::string&) override;
    NativeObservation ExecuteNative(Player&,const OperationRequest&) override;
    std::string PersistedNativeProof(Player&,const OperationRequest&,const Task&) const override;
private:
    QuestRewardQuote quote;
};
}
