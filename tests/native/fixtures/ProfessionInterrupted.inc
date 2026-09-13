#include "ProfessionInterrupted.h"
namespace {
void TestInterruptedProfession() {
    using namespace LivingActivity;using namespace LivingActivityTest;
    InterruptedFixture f;ProfessionPreparation result;std::string blocker;
    auto prepare=[&](const InterruptedFixture& x) {
        return PrepareInterruptedProfession(x.task,x.current,x.history,x.batch,x.frame,1200,x.receipt,result,blocker,x.bank,nullptr,x.carried);
    };
    assert(prepare(f));const auto accepted=result;
    assert(result.task.phase==Phase::Verifying && result.task.context==f.current && result.task.revision==5);
    assert(result.task.id==f.task.id && result.task.checkpoint.data==f.task.checkpoint.data);
    assert(result.task.retryAtMs==f.task.retryAtMs && result.task.checkpoint.activeElapsedMs==f.task.checkpoint.activeElapsedMs);
    for(const auto& sql:result.plan.statements) {
        assert(sql.find("UPDATE living_activity_claim")==std::string::npos);
        assert(sql.find("UPDATE characters ")==std::string::npos);
        assert(sql.find("INSERT INTO living_activity_operation")==std::string::npos);
    }
    auto fail=[&](const InterruptedFixture& x){assert(!prepare(x) && !blocker.empty() && result.plan.statements.empty());};
    auto bad=f;bad.task.context=f.current;fail(bad);
    bad=f;bad.task.context.actorGeneration=1;fail(bad);
    bad=f;bad.current.boot="invalid";fail(bad);
    bad=f;bad.current.actor=706;fail(bad);
    bad=f;bad.current.session="group:1";fail(bad);
    bad=f;bad.history.complete=false;fail(bad);
    bad=f;bad.history.interruptedCraft.reset();fail(bad);
    bad=f;bad.history.interruptedCraft->acknowledged=false;fail(bad);
    bad=f;bad.history.interruptedCraft->receipt.state=OperationState::Reconciling;fail(bad);
    bad=f;bad.history.interruptedCraft->receipt.nativeReference="possible_effect";fail(bad);
    bad=f;bad.history.interruptedCraft->afterState="{\"native\":1}";fail(bad);
    bad=f;bad.history.interruptedCraft->beforeState="{}";fail(bad);
    bad=f;++bad.history.interruptedCraft->receipt.taskRevision;fail(bad);
    bad=f;bad.history.interruptedCraft->receipt.task=f.receipt;fail(bad);
    bad=f;++bad.frame.money;fail(bad);
    bad=f;++bad.frame.skill;fail(bad);
    bad=f;++bad.frame.stacks[2].count;fail(bad);
    bad=f;--bad.frame.stacks[2].count;fail(bad);
    bad=f;++bad.frame.stacks[0].guid;fail(bad);
    bad=f;bad.frame.stacks.push_back({705,803,3371,1,0,26});assert(prepare(bad));
    std::swap(bad.frame.stacks[2],bad.frame.stacks[3]);fail(bad); // Changed consumption order is not permission to retry.
    bad=f;bad.batch.complete=false;fail(bad);
    bad=f;++bad.batch.claims[0].revision;fail(bad);
    bad=f;bad.batch.claims[0].state="consumed";fail(bad);
    bad=f;bad.batch.claims.push_back(bad.batch.claims[0]);fail(bad);
    auto storage=f;storage.AddBankDependency();assert(prepare(storage));
    assert(result.plan.statements[1].find("i.count=2")!=std::string::npos);
    bad=storage;bad.bank.clear();fail(bad);
    bad=storage;bad.bank[0].count=1;fail(bad);
    bad=storage;bad.bank[0].actor=706;fail(bad);
    bad=storage;bad.bank[0].slot=26;fail(bad);
    bad=storage;bad.batch.claims.back().location="mail";fail(bad);
    bad=storage;bad.batch.claims.back().state="reconciling";fail(bad);
    auto bagged=storage;bagged.bank[0].bagGuid=900;bagged.bank[0].slot=1;
    assert(prepare(bagged) && result.plan.statements[1].find("b.slot>=67 AND b.slot<74")!=std::string::npos);
    auto carried=f;carried.AddCarriedDependency();assert(prepare(carried));
    assert(result.plan.statements[1].find("v.item=100807")!=std::string::npos);
    bad=carried;bad.carried.clear();fail(bad);
    bad=carried;bad.carried[0].actor=706;fail(bad);
    bad=carried;bad.carried[0].entry=6217;fail(bad);
    bad=carried;bad.carried[0].count=0;fail(bad);
    bad=carried;bad.carried[0].slot=43;fail(bad);
    bad=carried;bad.carried.push_back(bad.carried[0]);fail(bad);
    bad=carried;bad.carried[0]=bad.frame.stacks[0];fail(bad);
    bad=carried;bad.batch.claims.back().state="in_transfer";fail(bad);
    bad=carried;bad.batch.claims.back().quantity=2;fail(bad);
    auto inBag=carried;inBag.carried[0].bagGuid=900;inBag.carried[0].slot=2;
    assert(prepare(inBag) && result.plan.statements[1].find("b.slot>=19 AND b.slot<23")!=std::string::npos);
    // An interrupted non-commit is stored without inventing a spell callback.
    auto row=*f.history.interruptedCraft;row.receipt.state=OperationState::Rejected;
    row.receipt.evidence="native_craft_intent_not_committed";
    row.receipt.nativeReference="spell:2329:operation:"+row.receipt.id;
    row.afterState="{\"recovery\":{\"version\":1,\"basis\":\"atomic_native_save_absent\",\"boot\":\""+
        f.current.boot+"\"},\"frame\":"+FrameJson(f.frame)+'}';
    StoredCraftProof proof;assert(DecodeStoredCraftProof(accepted.task,row,proof,blocker));
    assert(proof.attempt.committed && !proof.attempt.nativeEffectVerified && proof.gains.empty());
    assert(proof.attempt.skillBefore==proof.attempt.skillAfter && proof.inputs.size()==3);
    auto changed=row;changed.afterState="{}";assert(!DecodeStoredCraftProof(accepted.task,changed,proof,blocker));
    changed=row;changed.receipt.state=OperationState::Verified;assert(!DecodeStoredCraftProof(accepted.task,changed,proof,blocker));
    ProfessionHistoryRow fields={"705","4","1",row.receipt.id,f.task.id,"4","profession_craft","intent","",
        row.beforeState,"{}",""};
    ProfessionHistoryCursor cursor;assert(cursor.Begin(f.task,{fields},blocker) && cursor.Advance(blocker));
    assert(cursor.Result().complete && cursor.Result().unresolvedOperation && cursor.Result().interruptedCraft);
    // One paid portion is selected; another same-root purchase remains on the
    // same physical stack. Old intents cannot infer the missing before count.
    InterruptedFixture partial;ProfessionJob partialJob;InterruptedCraftIntent partialInput;
    assert(DecodeProfessionJob(partial.task.checkpoint.data,partialJob,blocker));
    auto partialIntent=*partial.history.interruptedCraft;
    assert(DecodeInterruptedCraftIntent(partial.task,partialIntent,partialInput,blocker));
    partialInput.inputs.back().before.quantity=2;
    auto unused=partialInput.inputs.back().before;
    unused.id="f71b6cac-8b42-44d9-9f1f-24e4e40a0009";unused.quantity=3;
    auto replaceIntent=[&](const std::string& native) {
        partialIntent.beforeState="{\"effects\":21,\"persistence\":2,\"native\":"+
            ClaimedNativeState(native,partialInput.inputs)+",\"item_gain\":"+ItemGainSpecJson(partialInput.output)+'}';
        partial.history.interruptedCraft=partialIntent;
    };
    partial.batch.claims.clear();
    for(const auto& use:partialInput.inputs)partial.batch.claims.push_back(use.before);
    partial.batch.claims.push_back(unused);
    replaceIntent("{\"recipe\":2329,\"skill\":1,\"money\":100}");
    assert(!prepare(partial)); // Keep conservative compatibility with old evidence.
    replaceIntent(EncodeCraftIntent(partialJob,partial.frame));
    assert(prepare(partial));const auto partialRecovered=result.task;
    // The real tool-preparation launch failure had a full atomic intent, then
    // an empty adapter exception. Keep the same operation and held purchases;
    // only an unchanged restored native frame permits non-commit recovery.
    auto exception=partial;
    exception.task.phase=Phase::Reconciling;++exception.task.revision;
    exception.task.checkpoint.blocker="native_adapter_exception";
    exception.history.revision=exception.task.revision;
    exception.history.interruptedCraft->receipt.state=OperationState::Reconciling;
    exception.history.interruptedCraft->receipt.evidence="native_adapter_exception";
    assert(prepare(exception));
    assert(result.task.id==exception.task.id && result.task.revision==exception.task.revision+1);
    for(const auto& sql:result.plan.statements) {
        assert(sql.find("UPDATE living_activity_claim")==std::string::npos);
        assert(sql.find("UPDATE item_instance")==std::string::npos);
        assert(sql.find(SqlValue("prior_observation"))==std::string::npos);
    }
    auto changedException=exception;changedException.task.context=exception.current;fail(changedException);
    changedException=exception;++changedException.frame.money;fail(changedException);
    changedException=exception;++changedException.frame.skill;fail(changedException);
    changedException=exception;++changedException.frame.stacks.back().count;fail(changedException);
    changedException=exception;--changedException.frame.stacks.back().count;fail(changedException);
    changedException=exception;++changedException.batch.claims.front().revision;fail(changedException);
    changedException=exception;changedException.history.interruptedCraft->afterState="{\"possible_effect\":true}";fail(changedException);
    changedException=exception;changedException.history.interruptedCraft->receipt.nativeReference="spell:2329";fail(changedException);
    changedException=exception;changedException.history.interruptedCraft->receipt.evidence="unknown_failure";fail(changedException);
    changedException=exception;--changedException.history.interruptedCraft->receipt.taskRevision;fail(changedException);
    changedException=exception;changedException.history.interruptedCraft->beforeState=f.history.interruptedCraft->beforeState;
    fail(changedException); // Old intents without a full frame are never relaxed.
    changedException=exception;
    auto& notAtomic=changedException.history.interruptedCraft->beforeState;
    auto atomicAt=notAtomic.find("\"persistence\":2");assert(atomicAt!=std::string::npos);
    notAtomic.replace(atomicAt,15,"\"persistence\":1");fail(changedException);
    auto badPartial=partial;--badPartial.frame.stacks.back().count;fail(badPartial);
    badPartial=partial;++badPartial.frame.stacks.back().count;fail(badPartial);
    badPartial=partial;badPartial.batch.claims.back().quantity=4;fail(badPartial);
    badPartial=partial;badPartial.batch.claims.back().actor=706;fail(badPartial);
    badPartial=partial;badPartial.batch.claims.back().location="bank";fail(badPartial);
    badPartial=partial;badPartial.batch.claims.erase(badPartial.batch.claims.begin());fail(badPartial);
    auto recoveredPartial=partialIntent;recoveredPartial.receipt.state=OperationState::Rejected;
    recoveredPartial.receipt.evidence="native_craft_intent_not_committed";
    recoveredPartial.receipt.nativeReference="spell:2329:operation:"+recoveredPartial.receipt.id;
    recoveredPartial.afterState="{\"recovery\":{\"version\":1,\"basis\":\"atomic_native_save_absent\",\"boot\":\""+
        partial.current.boot+"\"},\"frame\":"+FrameJson(partial.frame)+'}';
    assert(DecodeStoredCraftProof(partialRecovered,recoveredPartial,proof,blocker));
    assert(proof.inputs.back().before.quantity==2 && proof.inputs.back().used==1 && !proof.attempt.nativeEffectVerified);
    auto changedPartial=recoveredPartial;auto changedFrame=partial.frame;--changedFrame.stacks.back().count;
    changedPartial.afterState="{\"recovery\":{\"version\":1,\"basis\":\"atomic_native_save_absent\",\"boot\":\""+
        partial.current.boot+"\"},\"frame\":"+FrameJson(changedFrame)+'}';
    assert(!DecodeStoredCraftProof(partialRecovered,changedPartial,proof,blocker));
    // A paid merged stack still has separate claims after a crash before craft.
    InterruptedFixture merged;ProfessionJob job;
    assert(DecodeProfessionJob(merged.task.checkpoint.data,job,blocker));
    job.reagents.back().perAttempt=5;merged.task.checkpoint.data=EncodeProfessionJob(job);
    auto pending=SavedCraft(merged.task,job,merged.frame,merged.frame,true,true);
    pending.receipt.state=OperationState::Intent;pending.receipt.nativeReference.clear();
    pending.receipt.evidence.clear();pending.afterState="{}";
    InterruptedCraftIntent inputs;
    assert(DecodeInterruptedCraftIntent(merged.task,pending,inputs,blocker));
    auto second=inputs.inputs.back();inputs.inputs.back().before.quantity=inputs.inputs.back().used=2;
    second.before.id="f71b6cac-8b42-44d9-9f1f-24e4e40a0004";second.before.quantity=second.used=3;
    inputs.inputs.push_back(second);
    auto native="{\"recipe\":"+std::to_string(job.recipe)+",\"skill\":1,\"money\":100}";
    pending.beforeState="{\"effects\":21,\"persistence\":2,\"native\":"+ClaimedNativeState(native,inputs.inputs)+
        ",\"item_gain\":"+ItemGainSpecJson(inputs.output)+'}';
    merged.history.interruptedCraft=pending;merged.batch.claims.clear();
    for(const auto& use:inputs.inputs)merged.batch.claims.push_back(use.before);
    assert(prepare(merged));const auto mergedAccepted=result.task;
    auto missing=merged;missing.frame.stacks.back().count=4;assert(!prepare(missing));

    // On a later restart, the saved non-commit must remain readable with the
    // two paid claims on one physical stack, not just at its first recovery.
    auto recoveredMerged=pending;recoveredMerged.receipt.state=OperationState::Rejected;
    recoveredMerged.receipt.evidence="native_craft_intent_not_committed";
    recoveredMerged.receipt.nativeReference="spell:"+std::to_string(job.recipe)+":operation:"+pending.receipt.id;
    recoveredMerged.afterState="{\"recovery\":{\"version\":1,\"basis\":\"atomic_native_save_absent\",\"boot\":\""+
        merged.current.boot+"\"},\"frame\":"+FrameJson(merged.frame)+'}';
    assert(DecodeStoredCraftProof(mergedAccepted,recoveredMerged,proof,blocker));
    assert(proof.inputs.size()==4 && proof.attempt.committed && !proof.attempt.nativeEffectVerified);
    for(const auto count:{4u,6u}) {
        auto wrong=recoveredMerged;auto changedFrame=merged.frame;changedFrame.stacks.back().count=count;
        wrong.afterState="{\"recovery\":{\"version\":1,\"basis\":\"atomic_native_save_absent\",\"boot\":\""+
            merged.current.boot+"\"},\"frame\":"+FrameJson(changedFrame)+'}';
        assert(!DecodeStoredCraftProof(mergedAccepted,wrong,proof,blocker));
    }

    auto after=merged.frame;after.skill=2;after.stacks={{705,804,job.outputEntry,1,0,23}};
    auto success=SavedCraft(merged.task,job,merged.frame,after,false,true);
    success.beforeState=pending.beforeState;
    const auto cut=success.afterState.rfind(",\"claimed_consumption\":");assert(cut!=std::string::npos);
    const auto nativeAfter=success.afterState.substr(10,cut-10);
    success.afterState=ClaimedNativeState(nativeAfter,inputs.inputs,8192);
    auto completed=merged.task;++completed.revision;completed.phase=Phase::Verifying;
    assert(DecodeStoredCraftProof(completed,success,proof,blocker) && proof.inputs.size()==4);

    // Crash before a two-stack craft: same full before quantities, exact used
    // portions and no native saved effect. Recovery writes metadata only.
    InterruptedFixture split;auto splitJob=job;splitJob.reagents.back().perAttempt=7;
    split.task.checkpoint.data=EncodeProfessionJob(splitJob);
    split.frame.stacks.push_back({705,805,3371,20,900,0});
    auto splitIntent=SavedCraft(split.task,splitJob,split.frame,split.frame,true,true);
    splitIntent.receipt.state=OperationState::Intent;splitIntent.receipt.nativeReference.clear();
    splitIntent.receipt.evidence.clear();splitIntent.afterState="{}";
    assert(DecodeInterruptedCraftIntent(split.task,splitIntent,inputs,blocker) && inputs.inputs.size()==4);
    split.history.interruptedCraft=splitIntent;split.batch.claims.clear();
    for(const auto& input:inputs.inputs)split.batch.claims.push_back(input.before);
    assert(prepare(split));const auto splitRecovered=result.task;
    auto changedSplit=split;--changedSplit.frame.stacks.back().count;fail(changedSplit);
    changedSplit=split;std::swap(changedSplit.frame.stacks[2],changedSplit.frame.stacks[3]);fail(changedSplit);
    splitIntent.receipt.state=OperationState::Rejected;splitIntent.receipt.evidence="native_craft_intent_not_committed";
    splitIntent.receipt.nativeReference="spell:"+std::to_string(splitJob.recipe)+":operation:"+splitIntent.receipt.id;
    splitIntent.afterState="{\"recovery\":{\"version\":1,\"basis\":\"atomic_native_save_absent\",\"boot\":\""+
        split.current.boot+"\"},\"frame\":"+FrameJson(split.frame)+'}';
    assert(DecodeStoredCraftProof(splitRecovered,splitIntent,proof,blocker) && proof.inputs.size()==4);
    auto splitAfter=split.frame;splitAfter.skill=2;
    splitAfter.stacks={{705,805,3371,18,900,0},{705,806,splitJob.outputEntry,1,0,23}};
    auto splitSuccess=SavedCraft(split.task,splitJob,split.frame,splitAfter,false,true);
    assert(DecodeStoredCraftProof(splitRecovered,splitSuccess,proof,blocker) && proof.attempt.nativeEffectVerified);
}
}
