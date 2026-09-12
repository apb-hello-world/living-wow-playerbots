#include "LivingActivityOperations.h"
#include "LivingActivityTransfer.h"
#include <boost/property_tree/json_parser.hpp>
#include <sstream>
#include <cassert>
using namespace LivingActivity;
int main() {
    Task saved; saved.id = saved.root = "637bd562-36d2-5b01-bc01-e2d831c49f38";
    saved.actor = saved.context.actor = 497; saved.source = "service_job"; saved.sourceKey = "497:2881:41";
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
        request.transition.receipt,request.kind,"{\"effects\":"+std::to_string(request.effects)+",\"persistence\":0,\"native\":"+
        ClaimedNativeState(request.beforeState,request.consumption)+'}')));
    changed=request; changed.itemGain={3371,1};
    assert(!valid(changed,saved)); // Journal-only cannot acknowledge item gains.
    changed.persistence=NativePersistence::Inventory;
    assert(valid(changed,saved));
    auto gained=OperationRequestWrite(changed);
    assert(!SameRequest(gained,claimed));
    changed.itemGain.quantity=2; assert(!SameRequest(gained,OperationRequestWrite(changed)));
    changed.itemGain.entry=0; assert(!valid(changed,saved));
    changed=request; changed.consumption.front().used=11;
    assert(!SameRequest(claimed,OperationRequestWrite(changed)));
    changed=request; changed.persistence=NativePersistence::Inventory;
    assert(valid(changed,saved) && !SameRequest(claimed,OperationRequestWrite(changed)));
    changed.persistence=NativePersistence(99); assert(!valid(changed,saved));
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
    {
        auto transfer=request;transfer.consumption.clear();transfer.kind="bank_withdraw";
        transfer.effects=Mask(Effect::Inventory);transfer.persistence=NativePersistence::Inventory;
        transfer.itemTransfer=claim;transfer.itemTransfer.location="bank";
        ResourceClaimBook bank;assert(bank.RestoreBatch({transfer.itemTransfer})==ClaimInstall::Installed);
        assert(bank.FinishRestore());
        const std::vector<NativeResourceBalance> source={{saved.actor,45,2880,5,0,"bank"}};
        assert(valid(transfer,saved));
        assert(ValidateOperationResources(transfer,bank,source,reason));
        assert(!ValidateOperationResources(transfer,bank,stack,reason));
        auto altered=transfer;altered.itemTransfer.quantity=4;
        assert(!ValidateOperationResources(altered,bank,source,reason));
        altered=transfer;altered.consumption=request.consumption;assert(!valid(altered,saved));
        altered=transfer;altered.itemGain={2880,5};assert(!valid(altered,saved));
        altered=transfer;altered.itemTransfer.location="bags";assert(!valid(altered,saved));
        altered=transfer;altered.itemTransfer.actor++;assert(!valid(altered,saved));
        altered=transfer;altered.persistence=NativePersistence::JournalOnly;assert(!valid(altered,saved));
        boost::property_tree::ptree identity;std::istringstream json(BankTransferIdentity(transfer.itemTransfer));
        boost::property_tree::read_json(json,identity);
        assert(identity.get<std::string>("claim")==claim.id && identity.get<unsigned>("quantity")==5);
        auto verified=transfer.transition.task;++verified.revision;verified.phase=Phase::Verifying;
        OperationResult moved; moved.id=transfer.transition.receipt;moved.task=saved.id;
        moved.taskRevision=transfer.transition.task.revision;moved.kind="bank_withdraw";
        moved.state=OperationState::Verified;moved.nativeReference="bank_item:45";moved.evidence="native_bank_stack_relocated";
        auto write=BankTransferWrite(verified,transfer.transition.task.revision,moved,
            "ff2efbdf-f0ec-4539-b840-299847970c07","{}",transfer.itemTransfer);
        assert(write.changes.size()==1 && write.changes[0].after.id==claim.id &&
            write.changes[0].after.itemGuid==45 && write.changes[0].after.quantity==5 &&
            write.changes[0].after.location=="bags" && write.changes[0].after.state=="held");
        assert(write.journal.statements.back().find("c.location='bags'")!=std::string::npos);
        assert(bank.InstallReceipt(write.changes)==ClaimInstall::Installed);
        assert(bank.InstallReceipt(write.changes)==ClaimInstall::Duplicate);
        assert(bank.Protection().ProtectedItem(saved.actor,45,2880)==5);
        assert(!ValidateOperationResources(transfer,bank,source,reason)); // No repeated transfer after receipt.
        ResourceClaimBook restarted;assert(restarted.RestoreBatch({write.changes[0].after})==ClaimInstall::Installed);
        assert(restarted.FinishRestore());assert(restarted.Inspect(claim.id)->location=="bags");
        // Mail carries its exact envelope reference until the atomic transfer.
        // A matching item entry in another envelope never satisfies the intent.
        transfer.kind="mail_collect";transfer.itemTransfer.location="mail";transfer.itemTransfer.nativeReference=1234;
        ResourceClaimBook mail;assert(mail.RestoreBatch({transfer.itemTransfer})==ClaimInstall::Installed && mail.FinishRestore());
        const std::vector<NativeResourceBalance> attached={{saved.actor,45,2880,5,0,"mail",1234}};
        assert(valid(transfer,saved) && ValidateOperationResources(transfer,mail,attached,reason));
        auto wrongMail=attached;wrongMail[0].nativeReference=1235;
        assert(!ValidateOperationResources(transfer,mail,wrongMail,reason));
        altered=transfer;altered.kind="bank_withdraw";assert(!valid(altered,saved));
        altered=transfer;altered.itemTransfer.nativeReference=0;assert(!valid(altered,saved));
        moved.kind="mail_collect";moved.nativeReference="mail:1234:item:45";moved.evidence="native_mail_attachment_collected";
        const auto mailWrite=ItemTransferWrite(verified,transfer.transition.task.revision,moved,
            "ff2efbdf-f0ec-4539-b840-299847970c08","{}",transfer.itemTransfer);
        assert(mailWrite.changes[0].after.nativeReference==0 && mailWrite.changes[0].after.location=="bags");
        assert(mail.InstallReceipt(mailWrite.changes)==ClaimInstall::Installed);
        assert(mail.InstallReceipt(mailWrite.changes)==ClaimInstall::Duplicate);
        assert(!ValidateOperationResources(transfer,mail,attached,reason));
        ResourceClaimBook afterRestart;assert(afterRestart.RestoreBatch({mailWrite.changes[0].after})==ClaimInstall::Installed);
        assert(afterRestart.FinishRestore() && afterRestart.Inspect(claim.id)->nativeReference==0);
        const auto mergedWrite=ItemTransferWrite(verified,transfer.transition.task.revision,moved,
            "ff2efbdf-f0ec-4539-b840-299847970c09","{\"surviving_guid\":46,\"surviving_count\":12}",transfer.itemTransfer,46);
        assert(mergedWrite.changes[0].after.itemGuid==46 && mergedWrite.changes[0].after.quantity==5);
        assert(mergedWrite.changes[0].after.id==transfer.itemTransfer.id);
        assert(mergedWrite.journal.statements.front().find("c.item_guid=45")!=std::string::npos);
        assert(mergedWrite.journal.statements.back().find("SET c.item_guid=46")!=std::string::npos);
        assert(mergedWrite.journal.receiptQuery.find("c.item_guid=46")!=std::string::npos);
        // Capacity storage is the reverse native transfer, not consumption or
        // a synthetic item gain. The same claim follows the exact bank item.
        transfer.kind="bank_deposit";transfer.itemTransfer=claim;
        transfer.itemTransfer.location="bags";transfer.itemTransfer.nativeReference=0;
        ResourceClaimBook deposit;
        assert(deposit.RestoreBatch({transfer.itemTransfer})==ClaimInstall::Installed && deposit.FinishRestore());
        assert(valid(transfer,saved) && ValidateOperationResources(transfer,deposit,stack,reason));
        assert(!ValidateOperationResources(transfer,deposit,source,reason));
        altered=transfer;altered.kind="bank_withdraw";assert(!valid(altered,saved));
        altered=transfer;altered.itemTransfer.location="bank";assert(!valid(altered,saved));
        altered=transfer;altered.itemTransfer.nativeReference=1;assert(!valid(altered,saved));
        assert(ItemTransferIdentity(transfer.itemTransfer).find("\"destination\":\"bank\"")!=std::string::npos);
        moved.kind="bank_deposit";moved.evidence="native_bank_stack_deposited";
        const auto stored=ItemTransferWrite(verified,transfer.transition.task.revision,moved,
            "ff2efbdf-f0ec-4539-b840-299847970c10","{}",transfer.itemTransfer);
        assert(stored.changes[0].after.location=="bank" && stored.changes[0].after.state=="held");
        assert(stored.changes[0].after.itemGuid==45 && stored.changes[0].after.quantity==5);
        assert(stored.journal.statements.back().find("c.location='bank'")!=std::string::npos);
        assert(deposit.InstallReceipt(stored.changes)==ClaimInstall::Installed);
        assert(deposit.InstallReceipt(stored.changes)==ClaimInstall::Duplicate);
        assert(!ValidateOperationResources(transfer,deposit,stack,reason));
        ResourceClaimBook restoredDeposit;
        assert(restoredDeposit.RestoreBatch({stored.changes[0].after})==ClaimInstall::Installed && restoredDeposit.FinishRestore());
        assert(restoredDeposit.Inspect(claim.id)->location=="bank");
        assert(restoredDeposit.Protection().ProtectedItem(saved.actor,45,2880)==5);
    }
}
