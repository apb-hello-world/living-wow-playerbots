#pragma once
#include "LivingNativeGuildEvent.h"
#include "LivingActivityOperations.h"
namespace LivingActivity {
struct GuildGroupQuote {
    uint32_t actor=0,coordinator=0,group=0,leader=0,targetGroup=0,targetLeader=0;
    std::string change;
};
std::string EncodeGuildGroupQuote(const GuildGroupQuote&);
bool DecodeGuildGroupQuote(const std::string&,GuildGroupQuote&);
bool PlanNativeGuildGroup(Player&,const Task&,uint32_t,GuildGroupQuote&,std::string&);
// Native membership is idempotent desired state, unlike a purchase or craft.
// Recovery only inspects membership; it never resends an old invitation.
NativeObservation InspectNativeGuildGroup(Player&,const GuildGroupQuote&);
class NativeGuildGroup final : public NativeOperationAdapter {
public:
    explicit NativeGuildGroup(GuildGroupQuote value):quote(std::move(value)) {}
    const char* OperationKind() const override {return "guild_event_group";}
    uint32_t OperationEffects() const override {return Mask(Effect::Group);}
    bool ValidateNative(Player&,const OperationRequest&,std::string&) override;
    NativeObservation ExecuteNative(Player&,const OperationRequest&) override;
private:
    GuildGroupQuote quote;
};
}

