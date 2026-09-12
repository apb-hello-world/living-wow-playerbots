#include "ProfessionInterrupted.h"
namespace {
void TestInterruptedProfession() {
    using namespace LivingActivity;using namespace LivingActivityTest;
    InterruptedFixture f;ProfessionPreparation result;std::string blocker;
    auto prepare=[&](const InterruptedFixture& x) {
        return PrepareInterruptedProfession(x.task,x.current,x.history,x.batch,x.frame,1200,x.receipt,result,blocker,x.bank);
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
    bad=f;bad.frame.stacks.push_back({705,803,3371,1,0,26});fail(bad);
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
    assert(prepare(merged));
    auto missing=merged;missing.frame.stacks.back().count=4;assert(!prepare(missing));

    auto after=merged.frame;after.skill=2;after.stacks={{705,804,job.outputEntry,1,0,23}};
    auto success=SavedCraft(merged.task,job,merged.frame,after,false,true);
    success.beforeState=pending.beforeState;
    const auto cut=success.afterState.rfind(",\"claimed_consumption\":");assert(cut!=std::string::npos);
    const auto nativeAfter=success.afterState.substr(10,cut-10);
    success.afterState=ClaimedNativeState(nativeAfter,inputs.inputs,8192);
    auto completed=merged.task;++completed.revision;completed.phase=Phase::Verifying;
    assert(DecodeStoredCraftProof(completed,success,proof,blocker) && proof.inputs.size()==4);
}
}
