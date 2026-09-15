#pragma once
#include "LivingCommissionJob.h"
#include "LivingTaskItemRequirements.h"
namespace LivingActivityTest {
inline void TestCommissionCraftWorkflow() {
    ProfessionJob recipe;recipe.recipe=2329;recipe.skill=171;recipe.initialSkill=75;
    recipe.purpose=ProfessionPurpose::RequestedItem;recipe.outputEntry=2454;recipe.outputQuantity=1;
    recipe.reagents={{765,1},{2449,1},{3371,1}};
    CommissionJob job;job.agreement={"lwc-123","wow-tx-existing-offer","mail",EncodeProfessionJob(recipe),703,9,120,1000};
    job.craft=job.agreement.recipe;
    Task task;task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f92";task.actor=task.context.actor=703;
    task.source="commission_job";task.sourceKey=job.agreement.id;task.kind=Kind::Commission;
    task.mode=Mode::Active;task.priority=Priority::Human;task.phase=Phase::Verifying;task.revision=5;
    task.context.boot="40fdd3bf-585c-4409-9c33-72da5e77eef2";task.context.actorGeneration=2;task.context.mapGeneration=3;
    task.createdAtMs=task.updatedAtMs=1000;task.checkpoint.data=EncodeCommissionJob(job);
    std::string why;ProfessionWorkflow flow;std::vector<ProfessionReagent> needs;
    assert(ValidateProfessionTask(task,why) && IsProfessionJob(task));
    assert(DecodeProfessionWorkflow(task.checkpoint.data,flow,why));
    assert(EncodeTaskProfessionWorkflow(task,flow)==task.checkpoint.data);
    assert(ReadTaskItemRequirements(task,needs,why) && needs==recipe.reagents);
    assert(!ProfessionHistoryQuery(task).empty());
    for(unsigned i=0;i<6;++i) {
        auto changed=task;auto agreement=job;++changed.revision;
        if(i==0)agreement.agreement.recipient=10;
        if(i==1)agreement.agreement.feeCopper=0;
        if(i==2)agreement.agreement.delivery="direct";
        if(i==3)agreement.agreement.acceptedAtMs=1001;
        if(i==4)agreement.craftFinishedRevision=changed.revision;
        if(i==5)changed.phase=Phase::Completed;
        changed.checkpoint.data=EncodeCommissionJob(agreement);
        assert(!PreserveProfessionIntent(task,changed,why));
    }
    CommissionJob decoded;
    auto bad=task.checkpoint.data;const auto at=bad.find("profession_job_v1",bad.find("\"craft\""));
    assert(at!=std::string::npos);bad.replace(at,17,"commission_job_v1");
    assert(!DecodeCommissionJob(bad,decoded,why));
    bad=task.checkpoint.data;bad.insert(1,"\"craft_finished_revision\":0,");assert(!DecodeCommissionJob(bad,decoded,why));
    CraftFrame before;before.actor=703;before.money=100;before.skill=75;
    before.stacks={{703,100,765,1,0,23},{703,101,2449,1,0,24},{703,102,3371,5,0,25}};
    auto after=before;after.stacks={{703,102,3371,4,0,25},{703,103,2454,1,0,23}};
    auto evidence=SavedCraft(task,recipe,before,after,false,true);evidence.journalDigest=std::string(64,'a');
    StoredCraftProof proof;assert(DecodeStoredCraftProof(task,evidence,proof,why));
    ProfessionSnapshot snapshot;snapshot.task=task.id;snapshot.revision=task.revision;snapshot.context=task.context;
    snapshot.complete=true;snapshot.unresolvedOperation=false;snapshot.attempts={proof.attempt};snapshot.stock={{765},{2449},{3371}};
    ResourceClaim output;output.id=ItemGainClaimId(proof.attempt.receipt.id,103);output.task=task.id;output.actor=703;
    output.itemGuid=103;output.itemEntry=2454;output.quantity=1;output.state="held";output.location="bags";
    auto spare=output;spare.id="f90f9bb6-58c8-4d82-a3be-a25d3fec47b7";spare.itemGuid=102;spare.itemEntry=3371;spare.quantity=4;
    UnsettledClaimBatch batch{1,true,{output,spare}};
    std::vector<NativeResourceBalance> balances={{703,103,2454,1,0,"bags"},{703,102,3371,4,0,"bags"}};
    const std::string receipt="d1879146-6e96-4e71-827d-6d12d965edb7";ProfessionSettlement settled;
    auto prepare=[&]{return PrepareProfessionSettlement(task,snapshot,batch,balances,1001,receipt,settled,why);};
    {
        // Two actual receipts can contribute to one native stack and one
        // conserved folded claim. The receipt identities themselves survive.
        const auto savedTask=task;const auto savedSnapshot=snapshot;const auto savedBatch=batch;const auto savedBalances=balances;
        auto second=proof.attempt;second.receipt.id="2aa768f5-8234-4f22-9553-e4d7dbe2983b";
        second.receipt.taskRevision=7;second.receipt.nativeReference="spell:2329:operation:"+second.receipt.id;
        task.revision=snapshot.revision=8;snapshot.attempts.push_back(second);
        batch.claims[0].quantity=2;++batch.claims[0].revision;balances[0].quantity=2;
        if(!prepare())throw std::runtime_error("folded commission craft handoff: "+why);
        batch.claims[0].quantity=3;balances[0].quantity=3;
        assert(!prepare()); // Owned surplus is not another verified craft.
        batch.claims[0].quantity=2;balances[0].quantity=2;
        snapshot.attempts[1].gainedItems={{104,1}};
        assert(!prepare()); // Cannot borrow proof from a different stack.
        task=savedTask;snapshot=savedSnapshot;batch=savedBatch;balances=savedBalances;
    }
    if(!prepare())throw std::runtime_error("commission craft handoff: "+why);
    assert(settled.task.phase==Phase::Preparing && settled.task.checkpoint.step=="commission_craft_ready");
    assert(settled.claims.size()==1 && settled.claims[0].after.id==spare.id && settled.claims[0].after.state=="released");
    assert(DecodeCommissionJob(settled.task.checkpoint.data,decoded,why) && decoded.craftFinishedRevision==6);
    assert(ReadTaskItemRequirements(settled.task,needs,why) && needs==std::vector<ProfessionReagent>({{2454,1}}));
    assert(EncodeCommissionContract(decoded.agreement)==EncodeCommissionContract(job.agreement));
    for(const auto& sql:settled.plan.statements)assert(sql.find("SET money")==std::string::npos && sql.find("INSERT INTO mail")==std::string::npos);
    const auto good=settled;
    for(unsigned i=0;i<7;++i) {
        auto s=snapshot;auto b=batch;auto n=balances;
        if(i==0)snapshot.attempts.clear();
        if(i==1)snapshot.attempts[0].journalDigest.clear();
        if(i==2)snapshot.attempts[0].nativeEffectVerified=false;
        if(i==3)batch.claims[0].id=receipt;
        if(i==4)balances[0].location="bank";
        if(i==5)batch.complete=false;
        if(i==6)snapshot.unresolvedOperation=true;
        assert(!prepare());snapshot=s;batch=b;balances=n;
    }
    auto current=task.context;current.boot="1f6c3b70-d58f-4985-a67b-88eb30f6f943";current.policyRevision=2;snapshot.context=current;
    if(!PrepareProfessionRestartSettlement(task,current,snapshot,batch,balances,1001,receipt,settled,why))
        throw std::runtime_error("commission restart handoff: "+why);
    assert(settled.task.id==task.id && settled.task.context==current && settled.task.phase==Phase::Preparing);
    auto unvalidated=current;unvalidated.policyRevision=0;
    assert(!PrepareProfessionRestartSettlement(task,unvalidated,snapshot,batch,balances,1001,receipt,settled,why));
    auto forged=good.task;forged.phase=Phase::Completed;++forged.revision;
    assert(!PreserveProfessionIntent(good.task,forged,why)); // Craft is not delivery/payment.
}
}
