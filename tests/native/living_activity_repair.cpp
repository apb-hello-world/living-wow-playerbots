#include "LivingRepairQuote.h"
#include "LivingServiceTravel.h"
#include "LivingPartyRepair.h"
#include <cassert>
#include <limits>
#include <stdexcept>
using namespace LivingActivity;
int main() {
    uint32_t cost=0;
    assert(NativeRepairPrice(30,7,0.9,0.9f,cost) && cost==170);
    assert(NativeRepairPrice(1,0,0,1,cost) && cost==1);
    assert(!NativeRepairPrice(0,7,1,1,cost));
    assert(!NativeRepairPrice(UINT32_MAX,2,1,1,cost));
    assert(!NativeRepairPrice(1,1,-1,1,cost));
    assert(!NativeRepairPrice(1,1,std::numeric_limits<double>::quiet_NaN(),1,cost));
    assert(!NativeRepairPrice(1,1,1,0,cost));
    assert(!NativeRepairPrice(1,1,1,1.1f,cost));
    assert(!NativeRepairPrice(1,1,INT32_MAX,1,cost));
    for(uint32_t durability=1;durability<=100;++durability)
        for(uint32_t multiplier=1;multiplier<=31;++multiplier)
            for(const double quality:{0.5,0.9,1.0,1.2})
                for(const float discount:{0.8f,0.9f,1.0f}) {
                    assert(NativeRepairPrice(durability,multiplier,quality,discount,cost));
                    assert(cost==std::max(1u,uint32_t(uint32_t(durability*multiplier*quality)*discount)));
                }
    NativeRepairQuote q{444,251712,2506,30,0,1000,170,2113,998877,0xff11};
    const auto encoded=EncodeNativeRepairQuote(q);NativeRepairQuote decoded;
    assert(DecodeNativeRepairQuote(encoded,decoded) && EncodeNativeRepairQuote(decoded)==encoded);
    assert(!DecodeNativeRepairQuote(encoded+" ",decoded));
    assert(!DecodeNativeRepairQuote("{}",decoded));
    assert(VerifyNativeRepair(q,q.item,q.entry,q.position,q.maximum,30,830));
    assert(!VerifyNativeRepair(q,q.item,q.entry,q.position,q.maximum,0,1000));
    assert(!VerifyNativeRepair(q,q.item,q.entry,q.position,q.maximum,30,1000)); // Free repair is not proof.
    assert(!VerifyNativeRepair(q,q.item,q.entry,q.position,q.maximum,30,829));
    assert(!VerifyNativeRepair(q,q.item+1,q.entry,q.position,q.maximum,30,830));
    assert(!VerifyNativeRepair(q,q.item,q.entry,q.position+1,q.maximum,30,830));
    assert(!VerifyNativeRepair(q,q.item,q.entry,q.position,q.maximum+1,30,830));
    auto changed=q;changed.durability=1;assert(ValidNativeRepairQuote(changed));
    assert(DecodeNativeRepairQuote(EncodeNativeRepairQuote(changed),decoded) && decoded.durability==1);
    changed=q;changed.durability=changed.maximum;assert(!ValidNativeRepairQuote(changed));
    changed=q;changed.durability=changed.maximum+1;assert(!ValidNativeRepairQuote(changed));
    changed=q;changed.position=0xff13;assert(!ValidNativeRepairQuote(changed));
    changed=q;changed.money=100;assert(!ValidNativeRepairQuote(changed));
    ServiceDestination service;
    assert(ParseServiceStep("maintenance_service_repair",service) && service==ServiceDestination::Repair);
    assert(RepairPrerequisiteStep(ServiceStep(service)));
    Task t;t.id=t.root="12345678-1234-5234-9234-123456789abc";t.actor=444;t.mode=Mode::Active;
    t.phase=Phase::Traveling;t.checkpoint.step=ServiceStep(service);t.checkpoint.data="{\"retained_obligation\":1}";t.updatedAtMs=10;
    ServiceTravelResult arrived;arrived.arrived=true;Task next;
    assert(CheckpointServiceTravel(t,arrived,11,"maintenance_repair_prepare",next));
    assert(next.phase==Phase::Preparing && next.id==t.id && next.root==t.root && next.checkpoint.data==t.checkpoint.data);
    assert(next.phase!=Phase::Completed); // Reaching a repairer is never completion.
    ServiceTravelResult interrupted;interrupted.blocker="recipe_service_safety_pause";interrupted.safetyDetail="native_combat";
    assert(CheckpointServiceTravel(t,interrupted,11,"maintenance_repair_prepare",next));
    assert(next.phase==Phase::Paused && next.checkpoint.step==t.checkpoint.step && next.checkpoint.data==t.checkpoint.data);
    // Reproduce copied actor444 rev95: combat ended with a saved repair route.
    // A direct Paused -> Preparing request is invalid, even in the same context.
    for(const auto paused:{Phase::Paused,Phase::Deferred,Phase::WaitingExternal}) {
        Task interrupted=t;interrupted.phase=paused;
        assert(!CanTransition(interrupted,Phase::Preparing));
        const auto reconcile=RepairResumePhase(interrupted.phase);
        assert(reconcile && *reconcile==Phase::Reconciling && CanTransition(interrupted,*reconcile));
        interrupted.phase=*reconcile;
        const auto prepare=RepairResumePhase(interrupted.phase);
        assert(prepare && *prepare==Phase::Preparing && CanTransition(interrupted,*prepare));
        assert(interrupted.id==t.id && interrupted.checkpoint.data==t.checkpoint.data);
    }
    for(const auto phase:{Phase::Queued,Phase::Preparing,Phase::Traveling,Phase::Executing,
        Phase::Verifying,Phase::Completed,Phase::Failed,Phase::Cancelled})
        assert(!RepairResumePhase(phase)); // Atomic outcomes retain their own proof path.
    t.source="party_repair";t.sourceKey="repair-test";t.kind=Kind::PartyErrand;
    t.context.actor=t.actor;t.createdAtMs=1;
    t.checkpoint.data="{\"workflow\":\"party_repair_v1\"}";t.phase=Phase::Completed;t.revision=2;
    std::string why;assert(ValidatePartyRepairTask(t,why));
    auto invalid=t;invalid.checkpoint.data="{}";assert(!ValidatePartyRepairTask(invalid,why));
    invalid=t;invalid.kind=Kind::Profession;assert(!ValidatePartyRepairTask(invalid,why));
    OperationResult result;result.id="22222222-2222-4222-8222-222222222222";result.task=t.id;
    result.taskRevision=1;result.kind="critical_equipment_repair";result.state=OperationState::Verified;
    result.evidence="native_repair_durability_and_payment_observed";result.nativeReference="repair:123";
    const auto terminal=OperationOutcomeWrite(t,1,result,result.id,"{}");
    assert(!terminal.statements.empty());
    auto rejects=[&](const Task& candidate,const OperationResult& outcome) {
        bool threw=false;try{OperationOutcomeWrite(candidate,1,outcome,result.id,"{}");}
        catch(const std::invalid_argument&){threw=true;}assert(threw);
    };
    rejects(invalid,result);
    auto uncertain=result;uncertain.state=OperationState::Reconciling;rejects(t,uncertain);
    auto rejected=result;rejected.state=OperationState::Rejected;rejects(t,rejected);
    auto otherKind=result;otherKind.kind="mail_collect";rejects(t,otherKind);
}
