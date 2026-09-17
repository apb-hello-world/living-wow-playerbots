#include "LivingPartyVendor.h"
#include "LivingActivityRequests.h"
#include <cassert>
using namespace LivingActivity;
int main() {
    PartyVendorJob job{{{100,7074,3},{200,7073,1}},0},decoded;
    const auto encoded=EncodePartyVendorJob(job);
    assert(DecodePartyVendorJob(encoded,decoded) && decoded.next==0 && decoded.items.size()==2);
    for(const auto bad:{"{}", "[]", "null", "{\"workflow\":\"party_vendor_v2\"}", ""})
        assert(!DecodePartyVendorJob(bad,decoded));
    assert(!DecodePartyVendorJob(encoded+"{}",decoded));
    assert(!DecodePartyVendorJob(" "+encoded,decoded));
    auto extra=encoded;extra.insert(extra.size()-1,",\"next\":0");assert(!DecodePartyVendorJob(extra,decoded));
    auto changed=job;changed.next=3;assert(!DecodePartyVendorJob(EncodePartyVendorJob(changed),decoded));
    changed=job;changed.items[1].guid=100;assert(!ValidPartyVendorJob(changed));
    changed=job;changed.items[0].guid=300;assert(!ValidPartyVendorJob(changed));
    changed=job;changed.items[0].quantity=0;assert(!ValidPartyVendorJob(changed));
    changed=job;changed.items.clear();assert(!ValidPartyVendorJob(changed));
    changed={};for(uint32_t i=1;i<=32;++i)changed.items.push_back({i,7074,1});
    assert(DecodePartyVendorJob(EncodePartyVendorJob(changed),decoded));
    changed.items.push_back({33,7074,1});assert(!DecodePartyVendorJob(EncodePartyVendorJob(changed),decoded));
    Task task;task.id=task.root="11111111-1111-4111-8111-111111111111";task.source="party_vendor";
    task.sourceKey="party-vendor:7:1";task.actor=7;task.kind=Kind::PartyErrand;task.mode=Mode::Active;
    task.phase=Phase::Preparing;task.revision=4;task.context.actor=7;task.context.actorGeneration=1;
    task.createdAtMs=task.updatedAtMs=1000;
    task.context.mapGeneration=1;task.context.policyRevision=1;
    task.context.boot="22222222-2222-4222-8222-222222222222";
    task.checkpoint.step="party_vendor_prepare";task.checkpoint.data=encoded;std::string why;
    assert(ValidatePartyVendorTask(task,why));
    for(const auto phase:{Phase::Queued,Phase::Paused,Phase::Deferred,Phase::WaitingExternal,Phase::Reconciling}) {
        const auto next=PartyVendorResumePhase(phase);auto previous=task;previous.phase=phase;
        assert(next && CanTransition(previous,*next));
    }
    for(const auto phase:{Phase::Executing,Phase::Verifying,Phase::Completed})assert(!PartyVendorResumePhase(phase));
    auto badTask=task;badTask.kind=Kind::Profession;assert(!ValidatePartyVendorTask(badTask,why));
    badTask=task;badTask.parent="another";assert(!ValidatePartyVendorTask(badTask,why));
    badTask=task;badTask.checkpoint.step="profession_prepare";assert(!ValidatePartyVendorTask(badTask,why));
    badTask=task;badTask.phase=Phase::Completed;assert(!ValidatePartyVendorTask(badTask,why));
    TaskRequest transition;transition.task=task;++transition.task.revision;
    transition.expectedRevision=task.revision;transition.receipt=task.context.boot;
    assert(ValidateTaskRequest(transition,&task,task.context,why)==AdmissionCode::Pending);
    changed=job;changed.next=1;transition.task.checkpoint.data=EncodePartyVendorJob(changed);
    assert(ValidateTaskRequest(transition,&task,task.context,why)==AdmissionCode::InvalidRequest);
    changed=job;changed.items[0].quantity=2;transition.task.checkpoint.data=EncodePartyVendorJob(changed);
    assert(ValidateTaskRequest(transition,&task,task.context,why)==AdmissionCode::InvalidRequest);
    CapacitySaleFacts facts{7,100,7074,3,6,100,true,true,false,false};
    assert(PartyVendorItemMatches(job,facts));
    auto other=facts;++other.guid;assert(!PartyVendorItemMatches(job,other));
    other=facts;++other.entry;assert(!PartyVendorItemMatches(job,other));
    other=facts;++other.quantity;assert(!PartyVendorItemMatches(job,other));
    // Missing/replaced/merged stacks cannot advance from an aggregate bag delta.
    ResourceClaim claim;claim.id=task.context.boot;claim.task=task.id;claim.actor=7;
    claim.itemGuid=100;claim.itemEntry=7074;claim.quantity=3;claim.location="bags";claim.state="held";
    OperationResult proof;proof.id="33333333-3333-4333-8333-333333333333";proof.task=task.id;
    proof.taskRevision=4;proof.kind="party_vendor_sale";proof.state=OperationState::Verified;
    proof.evidence="native_party_sale_money_item_and_slot_observed";proof.nativeReference="vendor_sale:100";
    task.phase=Phase::Verifying;task.revision=5;task.checkpoint.step="party_vendor_sale";
    for(auto state:{OperationState::Intent,OperationState::Rejected,OperationState::Reconciling}) {
        auto wrong=proof;wrong.state=state;badTask=task;
        assert(!AcknowledgePartyVendorSale(badTask,facts,claim,wrong) && badTask.checkpoint.data==encoded);
    }
    auto wrong=proof;wrong.kind="capacity_vendor_sale";badTask=task;
    assert(!AcknowledgePartyVendorSale(badTask,facts,claim,wrong));
    wrong=proof;wrong.taskRevision=3;assert(!AcknowledgePartyVendorSale(badTask,facts,claim,wrong));
    wrong=proof;wrong.evidence="bag_space_increased";assert(!AcknowledgePartyVendorSale(badTask,facts,claim,wrong));
    auto wrongClaim=claim;wrongClaim.quantity=2;assert(!AcknowledgePartyVendorSale(badTask,facts,wrongClaim,proof));
    assert(AcknowledgePartyVendorSale(task,facts,claim,proof));
    assert(task.phase==Phase::Verifying && DecodePartyVendorJob(task.checkpoint.data,decoded) && decoded.next==1);
    assert(!AcknowledgePartyVendorSale(task,facts,claim,proof)); // Duplicate first receipt.
    auto restarted=AfterRestart(task,1000);assert(restarted.checkpoint.data==task.checkpoint.data);
    // Only the final exact acknowledged sale completes this fixed batch.
    facts.guid=claim.itemGuid=200;facts.entry=claim.itemEntry=7073;facts.quantity=claim.quantity=1;
    proof.taskRevision=8;proof.nativeReference="vendor_sale:200";task.revision=9;
    assert(AcknowledgePartyVendorSale(task,facts,claim,proof));
    assert(task.phase==Phase::Completed && ValidatePartyVendorTask(task,why));
    assert(!AcknowledgePartyVendorSale(task,facts,claim,proof));
}
