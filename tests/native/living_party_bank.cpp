#include "LivingPartyBank.h"
#include "LivingActivityRequests.h"
#include "LivingProfessionJob.h"
#include "LivingPartyService.h"
#include <cassert>
using namespace LivingActivity;
int main() {
    assert(!PartyBankFailureBackoff(false,OperationState::Rejected));
    assert(!PartyBankFailureBackoff(true,OperationState::Verified));
    assert(PartyBankFailureBackoff(true,OperationState::Rejected));
    PartyBankJob job{{{100,7074,3},{200,7073,1}},0},decoded;
    const auto encoded=EncodePartyBankJob(job);
    assert(DecodePartyBankJob(encoded,decoded) && decoded.next==0 && decoded.items.size()==2);
    for(const auto bad:{"{}", "[]", "null", "{\"workflow\":\"party_bank_v2\"}", ""})
        assert(!DecodePartyBankJob(bad,decoded));
    assert(!DecodePartyBankJob(encoded+"{}",decoded));
    assert(!DecodePartyBankJob(" "+encoded,decoded));
    auto extra=encoded;extra.insert(extra.size()-1,",\"next\":0");assert(!DecodePartyBankJob(extra,decoded));
    auto changed=job;changed.next=3;assert(!DecodePartyBankJob(EncodePartyBankJob(changed),decoded));
    changed=job;changed.items[1].guid=100;assert(!ValidPartyBankJob(changed));
    changed=job;changed.items[0].guid=300;assert(!ValidPartyBankJob(changed));
    changed=job;changed.items[0].quantity=0;assert(!ValidPartyBankJob(changed));
    changed=job;changed.items.clear();assert(!ValidPartyBankJob(changed));
    changed={};for(uint32_t i=1;i<=32;++i)changed.items.push_back({i,7074,1});
    assert(DecodePartyBankJob(EncodePartyBankJob(changed),decoded));
    changed.items.push_back({33,7074,1});assert(!DecodePartyBankJob(EncodePartyBankJob(changed),decoded));
    Task task;task.id=task.root="11111111-1111-4111-8111-111111111111";task.source="party_bank";
    task.sourceKey="party-vendor:7:1";task.actor=7;task.kind=Kind::PartyErrand;task.mode=Mode::Active;
    task.phase=Phase::Preparing;task.revision=4;task.context.actor=7;task.context.actorGeneration=1;
    task.createdAtMs=task.updatedAtMs=1000;
    task.context.mapGeneration=1;task.context.policyRevision=1;
    task.context.boot="22222222-2222-4222-8222-222222222222";
    task.checkpoint.step="party_bank_prepare";task.checkpoint.data=encoded;std::string why;
    assert(ValidatePartyBankTask(task,why));
    for(const auto phase:{Phase::Queued,Phase::Paused,Phase::Deferred,Phase::WaitingExternal,Phase::Reconciling}) {
        const auto next=PartyBankResumePhase(phase);auto previous=task;previous.phase=phase;
        assert(next && CanTransition(previous,*next));
    }
    for(const auto phase:{Phase::Executing,Phase::Verifying,Phase::Completed})assert(!PartyBankResumePhase(phase));
    auto badTask=task;badTask.kind=Kind::Profession;assert(!ValidatePartyBankTask(badTask,why));
    badTask=task;badTask.parent="another";assert(!ValidatePartyBankTask(badTask,why));
    badTask=task;badTask.checkpoint.step="profession_prepare";assert(!ValidatePartyBankTask(badTask,why));
    badTask=task;badTask.phase=Phase::Completed;assert(!ValidatePartyBankTask(badTask,why));
    TaskRequest transition;transition.task=task;++transition.task.revision;
    transition.expectedRevision=task.revision;transition.receipt=task.context.boot;
    assert(ValidateTaskRequest(transition,&task,task.context,why)==AdmissionCode::Pending);
    // Shared route labels must not reclassify this party root as a recipe job.
    auto travel=transition;travel.task.phase=Phase::Traveling;
    travel.task.checkpoint.step="profession_service_bank";
    assert(!IsProfessionJob(travel.task));
    assert(ValidateTaskRequest(travel,&task,task.context,why)==AdmissionCode::Pending);
    assert(SavedTaskExecutable(travel.task,travel.task.revision,task.context,1000,why));
    auto malformed=travel.task;malformed.source="profession_job";
    assert(IsProfessionJob(malformed) && !ValidateProfessionTask(malformed,why));
    changed=job;changed.next=1;transition.task.checkpoint.data=EncodePartyBankJob(changed);
    assert(ValidateTaskRequest(transition,&task,task.context,why)==AdmissionCode::InvalidRequest);
    changed=job;changed.items[0].quantity=2;transition.task.checkpoint.data=EncodePartyBankJob(changed);
    assert(ValidateTaskRequest(transition,&task,task.context,why)==AdmissionCode::InvalidRequest);
    PartyBankItem facts{100,7074,3};
    assert(PartyBankItemMatches(job,facts));
    auto other=facts;++other.guid;assert(!PartyBankItemMatches(job,other));
    other=facts;++other.entry;assert(!PartyBankItemMatches(job,other));
    other=facts;++other.quantity;assert(!PartyBankItemMatches(job,other));
    // Missing/replaced/merged stacks cannot advance from an aggregate bag delta.
    ResourceClaim claim;claim.id=task.context.boot;claim.task=task.id;claim.actor=7;
    claim.itemGuid=100;claim.itemEntry=7074;claim.quantity=3;claim.location="bags";claim.state="held";
    OperationResult proof;proof.id="33333333-3333-4333-8333-333333333333";proof.task=task.id;
    proof.taskRevision=4;proof.kind="bank_deposit";proof.state=OperationState::Verified;
    proof.evidence="native_bank_stack_deposited";proof.nativeReference="bank_item:100";
    task.phase=Phase::Verifying;task.revision=5;task.checkpoint.step="bank_deposit";
    for(auto state:{OperationState::Intent,OperationState::Rejected,OperationState::Reconciling}) {
        auto wrong=proof;wrong.state=state;badTask=task;
        assert(!AcknowledgePartyBankDeposit(badTask,facts,claim,wrong) && badTask.checkpoint.data==encoded);
    }
    auto wrong=proof;wrong.kind="capacity_vendor_sale";badTask=task;
    assert(!AcknowledgePartyBankDeposit(badTask,facts,claim,wrong));
    wrong=proof;wrong.taskRevision=3;assert(!AcknowledgePartyBankDeposit(badTask,facts,claim,wrong));
    wrong=proof;wrong.evidence="bag_space_increased";assert(!AcknowledgePartyBankDeposit(badTask,facts,claim,wrong));
    auto wrongClaim=claim;wrongClaim.quantity=2;assert(!AcknowledgePartyBankDeposit(badTask,facts,wrongClaim,proof));
    assert(AcknowledgePartyBankDeposit(task,facts,claim,proof));
    assert(task.phase==Phase::Verifying && DecodePartyBankJob(task.checkpoint.data,decoded) && decoded.next==1);
    assert(!AcknowledgePartyBankDeposit(task,facts,claim,proof)); // Duplicate first receipt.
    auto restarted=AfterRestart(task,1000);assert(restarted.checkpoint.data==task.checkpoint.data);
    // Only the final exact acknowledged sale completes this fixed batch.
    facts.guid=claim.itemGuid=200;facts.entry=claim.itemEntry=7073;facts.quantity=claim.quantity=1;
    proof.taskRevision=8;proof.nativeReference="bank_item:200";task.revision=9;
    assert(AcknowledgePartyBankDeposit(task,facts,claim,proof));
    assert(task.phase==Phase::Completed && ValidatePartyBankTask(task,why));
    // Exercise the actual journal boundary, not only cursor acknowledgement.
    // Final sale, task completion and consumed claim share this native write.
    const auto journal=OperationOutcomeWrite(task,8,proof,task.context.boot,"{}");
    assert(!journal.statements.empty() && !journal.receiptQuery.empty());
    auto rejectsFinal=[&](const Task& candidate,const OperationResult& result) {
        bool rejected=false;
        try { OperationOutcomeWrite(candidate,8,result,task.context.boot,"{}"); }
        catch(const std::invalid_argument&) { rejected=true; }
        assert(rejected);
    };
    wrong=proof;wrong.nativeReference="bank_item:100";rejectsFinal(task,wrong);
    wrong=proof;wrong.kind="capacity_vendor_sale";rejectsFinal(task,wrong);
    wrong=proof;wrong.state=OperationState::Rejected;rejectsFinal(task,wrong);
    wrong=proof;wrong.taskRevision=7;rejectsFinal(task,wrong);
    badTask=task;decoded.next=1;badTask.checkpoint.data=EncodePartyBankJob(decoded);rejectsFinal(badTask,proof);
    assert(!AcknowledgePartyBankDeposit(task,facts,claim,proof));
    // A deposit releases storage-only protection in the same journal. It does
    // not destroy/consume the actual banked item, and does not free it before ACK.
    const auto transfer=ItemTransferWrite(task,8,proof,task.context.boot,"{}",claim,claim.itemGuid,true);
    assert(transfer.changes.size()==1 && transfer.changes[0].after.state=="released");
    assert(transfer.changes[0].after.location=="bank" && transfer.changes[0].after.quantity==1);
    ResourceClaimBook book(16);
    assert(book.RestoreBatch({claim})==ClaimInstall::Installed && book.FinishRestore());
    NativeResourceBalance balance{7,200,7073,1,0,"bank"};
    assert(book.ReserveTransferred(task.context.boot,transfer.changes.front(),balance)==ClaimInstall::Installed);
    assert(book.Protection().ProtectedItem(7,200,7073)==1);
    assert(book.CommitReservation(task.context.boot)==ClaimInstall::Installed);
    assert(book.Protection().ProtectedItem(7,200,7073)==0);
    // A merge needs protection on the surviving bank identity until save ACK.
    ResourceClaimBook merged(16);
    assert(merged.RestoreBatch({claim})==ClaimInstall::Installed && merged.FinishRestore());
    auto remapped=transfer.changes.front();remapped.after.itemGuid=300;
    balance.itemGuid=300;balance.quantity=5;
    assert(merged.ReserveTransferred(task.context.boot,remapped,balance)==ClaimInstall::Installed);
    assert(merged.Protection().ProtectedItem(7,200,7073)==1 && merged.Protection().ProtectedItem(7,300,7073)==1);
    assert(merged.CommitReservation(task.context.boot)==ClaimInstall::Installed);
    assert(!merged.Protection().ProtectedItem(7,200,7073) && !merged.Protection().ProtectedItem(7,300,7073));
    PartyServiceBinding binding;binding.root=task.id;binding.actor=7;binding.human=1;
    binding.session="group:1:2";binding.sessionRevision=3;binding.acceptedRevision=1;
    binding.service=PartyServiceBinding::Service::Bank;
    auto active=task;active.phase=Phase::Preparing;active.accepted=true;
    PartyServiceWindow window{7,1,"group:1:2",3,true};
    assert(PartyServiceMatches(binding,active,window));
    ++window.sessionRevision;assert(!PartyServiceMatches(binding,active,window));
    assert(PartyServiceOperation(binding,"bank_deposit",claim));
    assert(!PartyServiceOperation(binding,"bank_withdraw",claim));
    assert(!PartyServiceOperation(binding,"guild_bank_deposit",claim));
    assert(PartyServiceEffects(Mask(Effect::Inventory),binding.service));
    assert(!PartyServiceEffects(Mask(Effect::Money),binding.service));
    assert(PartyServiceReceipt(binding,task,"bank_deposit",claim,"saved"));
    assert(!PartyServiceReceipt(binding,task,"bank_deposit",claim,"duplicate"));
}
