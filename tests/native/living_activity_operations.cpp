#include "LivingActivityOperations.h"
#include <cassert>
using namespace LivingActivity;
int main() {
    Task saved; saved.id = saved.root = "637bd562-36d2-5b01-bc01-e2d831c49f38";
    saved.actor = saved.context.actor = 497; saved.source = "profession_job"; saved.sourceKey = "497:2881:41";
    saved.context.boot = "ff2efbdf-f0ec-4539-b840-299847970c00";
    saved.context.actorGeneration = saved.context.mapGeneration = saved.context.policyRevision = 1;
    saved.mode = Mode::Active; saved.phase = Phase::Preparing;
    saved.createdAtMs = saved.updatedAtMs = 1000;
    OperationRequest request; request.transition.task = saved; request.transition.task.phase = Phase::Executing;
    request.transition.expectedRevision = saved.revision; ++request.transition.task.revision;
    request.transition.receipt = "ff2efbdf-f0ec-4539-b840-299847970c01";
    request.kind = "vendor_purchase"; request.effects = Mask(Effect::Inventory) | Mask(Effect::Money);
    request.beforeState = "{\"item\":2880,\"quantity\":1,\"money\":100}";
    auto& action = request.authorization; action.task = action.rootTask = saved.id;
    action.world = saved.context; action.revision = saved.revision; action.ownerGeneration = 7;
    action.origin = "vendor_adapter"; action.permittedEffects = request.effects;
    std::string reason;
    auto valid = [&](const OperationRequest& r, const Task& task) {
        return ValidateOperationRequest(r, task, saved.context, nullptr, 1000, reason);
    };
    assert(valid(request, saved));
    const auto plan = OperationRequestWrite(request);
    assert(SameRequest(plan, OperationRequestWrite(request)));
    auto changed = request; changed.beforeState = "{\"money\":1000}";
    assert(!SameRequest(plan, OperationRequestWrite(changed)));
    changed = request; changed.effects = Mask(Effect::Inventory);
    assert(!SameRequest(plan, OperationRequestWrite(changed)));
    for (unsigned field = 0; field < 8; ++field) {
        changed = request;
        switch (field) {
        case 0: ++changed.authorization.revision; break;
        case 1: changed.authorization.ownerGeneration = 0; break;
        case 2: changed.authorization.permittedEffects = Mask(Effect::Inventory); break;
        case 3: changed.authorization.operation = request.transition.receipt; break;
        case 4: changed.effects = 512; break;
        case 5: changed.kind = "not an operation"; break;
        case 6: changed.beforeState = "[]"; break;
        default: changed.transition.task.context.boot.clear();
        }
        assert(!valid(changed, saved));
    }
    auto blocked = saved; blocked.retryAtMs = 1001; assert(!valid(request, blocked));
    blocked = saved; blocked.checkpoint.blocker = "paid_mail_pending"; assert(!valid(request, blocked));
    blocked = AfterRestart(saved, 2000); assert(!valid(request, blocked));
    NativeObservation outcome;
    assert(ValidateNativeObservation(outcome)); // Uncertain by default, never success.
    outcome.state = OperationState::Verified; assert(!ValidateNativeObservation(outcome));
    outcome.nativeReference = "native_vendor_receipt:41"; outcome.evidence = "native_purchase_verified";
    assert(ValidateNativeObservation(outcome));
    outcome.afterState = "invalid"; assert(!ValidateNativeObservation(outcome));
    outcome.afterState = "{}"; outcome.state = OperationState::Intent; assert(!ValidateNativeObservation(outcome));
    outcome.state = OperationState::Rejected; outcome.nativeReference.clear(); assert(ValidateNativeObservation(outcome));
    // Unknown effects retain their native reconciliation reference and after-state.
    outcome.state = OperationState::Reconciling; outcome.nativeReference = "native_mail:81";
    outcome.afterState = "{\"attachment_seen\":true}";
    assert(ValidateNativeObservation(outcome));
    auto reconciling = request.transition.task; ++reconciling.revision; reconciling.phase = Phase::Reconciling;
    OperationResult proof; proof.id = request.transition.receipt; proof.task = saved.id;
    proof.taskRevision = request.transition.task.revision; proof.kind = request.kind;
    proof.state = outcome.state; proof.nativeReference = outcome.nativeReference; proof.evidence = outcome.evidence;
    const auto uncertainty = OperationOutcomeWrite(reconciling, request.transition.task.revision, proof,
        "ff2efbdf-f0ec-4539-b840-299847970c02", outcome.afterState);
    assert(uncertainty.receiptQuery.find("after_state=") != std::string::npos);
    assert(uncertainty.statements.back().find(SqlValue("native_mail:81")) != std::string::npos);
    auto observed = proof; observed.evidence = "different_uncertainty";
    assert(!SameRequest(uncertainty, OperationOutcomeWrite(reconciling, request.transition.task.revision, observed,
        "ff2efbdf-f0ec-4539-b840-299847970c02", outcome.afterState)));

    ResourceClaim claim; claim.id="ff2efbdf-f0ec-4539-b840-299847970c03"; claim.task=saved.id;
    claim.actor=saved.actor; claim.copper=30; claim.location="money"; claim.state="held"; claim.revision=1;
    ResourceClaimBook book; assert(book.RestoreBatch({claim}) == ClaimInstall::Installed); assert(book.FinishRestore());
    request.consumption={{claim,10}};
    assert(valid(request,saved));
    const auto claimed=OperationRequestWrite(request);
    assert(SameRequest(claimed,OperationIntentWrite(request.transition.task,request.transition.expectedRevision,
        request.transition.receipt,request.kind,"{\"effects\":"+std::to_string(request.effects)+",\"native\":"+
        ClaimedNativeState(request.beforeState,request.consumption)+'}')));
    changed=request; changed.consumption.front().used=11;
    assert(!SameRequest(claimed,OperationRequestWrite(changed)));
    changed=request; changed.consumption.front().before.actor++;
    assert(!valid(changed,saved));
    changed=request; changed.effects=Mask(Effect::Inventory); assert(!valid(changed,saved));
    changed=request; changed.consumption.push_back(changed.consumption.front()); assert(!valid(changed,saved));
    const std::vector<NativeResourceBalance> wallet={{saved.actor,0,0,0,100,"money"}};
    assert(ValidateOperationResources(request,book,wallet,reason));
    changed=request; ++changed.consumption.front().before.revision;
    assert(!ValidateOperationResources(changed,book,wallet,reason) && reason == "acknowledged_claim_changed");
    assert(!ValidateOperationResources(request,book,{},reason) && reason == "claimed_native_resource_unavailable");
    assert(!ValidateOperationResources(request,book,{{saved.actor,0,0,0,29,"money"}},reason));
    auto other=claim; other.id="ff2efbdf-f0ec-4539-b840-299847970c04";
    other.task="ff2efbdf-f0ec-4539-b840-299847970c05"; other.copper=50;
    assert(book.ReservePending("ff2efbdf-f0ec-4539-b840-299847970c06",{{other,0}},wallet) == ClaimInstall::Installed);
    assert(!ValidateOperationResources(request,book,{{saved.actor,0,0,0,79,"money"}},reason));
    assert(ValidateOperationResources(request,book,wallet,reason));
    assert(VerifyConsumedNativeResources(request,wallet,{{saved.actor,0,0,0,90,"money"}},reason));
    assert(!VerifyConsumedNativeResources(request,wallet,wallet,reason) && reason == "native_consumption_delta_mismatch");
    assert(!VerifyConsumedNativeResources(request,wallet,{},reason) && reason == "native_consumption_proof_missing");
    assert(!VerifyConsumedNativeResources(request,wallet,{{saved.actor,0,0,0,89,"money"}},reason));
    book.BlockProjection(); assert(!ValidateOperationResources(request,book,wallet,reason));
    claim.itemGuid=45; claim.itemEntry=2880; claim.quantity=5; claim.copper=0; claim.location="bags";
    request.consumption={{claim,5}};
    const std::vector<NativeResourceBalance> stack={{saved.actor,45,2880,5,0,"bags"}};
    assert(VerifyConsumedNativeResources(request,stack,{},reason));
    request.consumption.front().used=2;
    assert(VerifyConsumedNativeResources(request,stack,{{saved.actor,45,2880,3,0,"bags"}},reason));
    assert(!VerifyConsumedNativeResources(request,stack,{},reason));
}
