#pragma once
#include "LivingPartyTraining.h"
#include "LivingActivityOperations.h"
class ObjectGuid;
namespace LivingActivity {
bool PlanNativePartyTraining(Player&,const ObjectGuid&,PartyTrainingJob&,std::string&);
bool QuoteNativePartyTraining(Player&,const Task&,TrainingLessonQuote&,std::string&);
class NativePartyTraining final:public NativeOperationAdapter {
public:
    explicit NativePartyTraining(TrainingLessonQuote value):quote(std::move(value)){}
    const char* OperationKind() const override{return "party_training_learn";}
    uint32_t OperationEffects() const override{return Mask(Effect::Spell)|Mask(Effect::Social);}
    NativePersistence PersistencePolicy() const override{return NativePersistence::Profession;}
    bool ValidateNative(Player&,const OperationRequest&,std::string&) override;
    NativeObservation ExecuteNative(Player&,const OperationRequest&) override;
    std::string PersistedNativeProof(Player&,const OperationRequest&,const Task&) const override;
private:
    TrainingLessonQuote quote;
};
}
