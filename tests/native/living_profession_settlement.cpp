#include "LivingProfessionSettlement.h"
#include "fixtures/CraftEvidence.h"
#include <cassert>
using namespace LivingActivity;
using namespace LivingActivityTest;
#include "fixtures/ProfessionAttempt.inc"
#include "fixtures/ProfessionResume.inc"
#include "fixtures/ProfessionInterrupted.inc"
#include "fixtures/MailRecovery.inc"
#include "fixtures/EnchantWorkflow.inc"
int main() {
    TestProfessionAttemptPlan();
    TestProfessionResumption();
    TestInterruptedProfession();
    TestMailRecovery();
    TestEnchantWorkflow();
    Task task;task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f92";
    task.actor=task.context.actor=703;task.source="profession_job";task.sourceKey="settlement_fixture";
    task.mode=Mode::Active;task.phase=Phase::Verifying;task.kind=Kind::Profession;task.revision=5;
    task.createdAtMs=task.updatedAtMs=1000;task.context.boot="test_boot";
    ProfessionJob job;job.recipe=2329;job.skill=171;job.initialSkill=1;job.targetSkill=2;
    job.outputEntry=2454;job.outputQuantity=1;job.reagents={{765,1},{2449,1},{3371,1}};
    task.checkpoint.data=EncodeProfessionJob(job);
    CraftFrame before;before.actor=703;before.money=100;before.skill=1;
    before.stacks={{703,100,765,1,0,23},{703,101,2449,1,0,24},{703,102,3371,5,0,25}};
    auto after=before;after.skill=2;after.stacks={{703,102,3371,4,0,25},{703,103,2454,1,0,23}};
    const auto evidence=SavedCraft(task,job,before,after,false,true);
    StoredCraftProof proof;std::string blocker;assert(DecodeStoredCraftProof(task,evidence,proof,blocker));
    ProfessionSnapshot snapshot;snapshot.task=task.id;snapshot.revision=task.revision;snapshot.context=task.context;
    snapshot.complete=true;snapshot.unresolvedOperation=false;snapshot.attempts={proof.attempt};
    snapshot.stock={{765},{2449},{3371}};
    ResourceClaim surplus;surplus.id="ff2efbdf-f0ec-4539-b840-299847972001";surplus.task=task.id;surplus.actor=703;
    surplus.itemGuid=102;surplus.itemEntry=3371;surplus.quantity=4;surplus.state="held";surplus.location="bags";surplus.revision=2;
    auto output=surplus;output.id="ff2efbdf-f0ec-4539-b840-299847972002";output.itemGuid=103;output.itemEntry=2454;
    output.quantity=1;output.revision=1;
    ResourceClaimBook book;assert(book.RestoreBatch({surplus,output})==ClaimInstall::Installed && book.FinishRestore());
    UnsettledClaimBatch batch;assert(book.ReadUnsettled(task.id,batch,blocker) && batch.complete && batch.claims.size()==2);
    const std::vector<NativeResourceBalance> balances={{703,102,3371,4,0,"bags"},{703,103,2454,1,0,"bags"}};
    ProfessionSettlement settled;
    const std::string receipt="ff2efbdf-f0ec-4539-b840-299847972003";
    auto prepare=[&](const Task& owner,const ProfessionSnapshot& view,const UnsettledClaimBatch& rows,
        const std::vector<NativeResourceBalance>& native){return PrepareProfessionSettlement(owner,view,rows,native,1001,receipt,settled,blocker);};
    assert(prepare(task,snapshot,batch,balances) && blocker.empty());
    assert(settled.task.phase==Phase::Completed && settled.task.revision==6 && settled.claims.size()==2);
    assert(settled.task.checkpoint.data==task.checkpoint.data && settled.task.checkpoint.blocker.empty());
    assert(book.Protection().ProtectedItem(703,102,3371)==4); // Queueing is not acknowledgement.
    const auto completed=settled;
    assert(completed.plan.statements.front().find("actor_guid=actor_guid")!=std::string::npos);
    assert(completed.plan.statements[1].find(SqlValue("$.native.result.after.skill"))!=std::string::npos);
    assert(completed.plan.statements[1].find("c.state NOT IN ('consumed','released')")!=std::string::npos);
    WorldContext restarted=task.context;restarted.boot="ff2efbdf-f0ec-4539-b840-299847974001";
    restarted.actorGeneration=19;restarted.mapGeneration=27;restarted.policyRevision=3;
    auto recoveredSnapshot=snapshot;recoveredSnapshot.context=restarted;
    auto recover=[&](const WorldContext& context,const ProfessionSnapshot& view) {
        return PrepareProfessionRestartSettlement(task,context,view,batch,balances,1001,receipt,settled,blocker);
    };
    assert(recover(restarted,recoveredSnapshot));
    assert(settled.task.phase==Phase::Completed && settled.task.context==restarted &&
        settled.task.checkpoint.data==task.checkpoint.data && settled.task.id==task.id &&
        settled.task.revision==task.revision+1 && task.context.boot=="test_boot");
    assert(settled.plan.statements[1].find("AND phase='verifying'")!=std::string::npos &&
        settled.plan.statements[1].find("o.state IN ('intent','reconciling')")!=std::string::npos);
    {
        auto zoned=task;zoned.context=restarted;auto arrived=restarted;++arrived.mapGeneration;
        auto fresh=recoveredSnapshot;fresh.context=arrived;
        assert(PrepareProfessionRestartSettlement(zoned,arrived,fresh,batch,balances,1001,receipt,settled,blocker));
        assert(settled.task.phase==Phase::Completed && settled.task.context==arrived);
    }
    auto recoveryFail=[&](const WorldContext& context,const ProfessionSnapshot& view) {
        assert(!recover(context,view) && !blocker.empty() && settled.plan.statements.empty() && settled.claims.empty());
    };
    recoveryFail(restarted,snapshot); // Old worker snapshot cannot be reused.
    auto wrongContext=restarted;wrongContext.actor=704;recoveryFail(wrongContext,recoveredSnapshot);
    wrongContext=restarted;wrongContext.boot=task.context.boot;recoveryFail(wrongContext,recoveredSnapshot);
    wrongContext=restarted;wrongContext.boot="";recoveryFail(wrongContext,recoveredSnapshot);
    wrongContext=restarted;wrongContext.actorGeneration=0;recoveryFail(wrongContext,recoveredSnapshot);
    wrongContext=restarted;wrongContext.mapGeneration=0;recoveryFail(wrongContext,recoveredSnapshot);
    wrongContext=restarted;wrongContext.policyRevision=0;recoveryFail(wrongContext,recoveredSnapshot);
    wrongContext=restarted;wrongContext.session="group:3";recoveryFail(wrongContext,recoveredSnapshot);
    wrongContext=restarted;wrongContext.sessionRevision=1;recoveryFail(wrongContext,recoveredSnapshot);
    auto uncertain=recoveredSnapshot;uncertain.unresolvedOperation=true;recoveryFail(restarted,uncertain);
    uncertain=recoveredSnapshot;uncertain.attempts.clear();recoveryFail(restarted,uncertain);
    uncertain=recoveredSnapshot;uncertain.attempts[0].nativeEffectVerified=false;recoveryFail(restarted,uncertain);
    auto restored=task;restored.context.boot.clear();restored.context.actorGeneration=restored.context.mapGeneration=0;
    assert(PrepareProfessionRestartSettlement(restored,restarted,recoveredSnapshot,batch,balances,1001,receipt,settled,blocker));
    restored.context.actorGeneration=1;
    assert(!PrepareProfessionRestartSettlement(restored,restarted,recoveredSnapshot,batch,balances,1001,receipt,settled,blocker));
    auto fail=[&](const Task& owner,const ProfessionSnapshot& view,const UnsettledClaimBatch& rows,
        const std::vector<NativeResourceBalance>& native){assert(!prepare(owner,view,rows,native));
        assert(!blocker.empty() && settled.plan.statements.empty() && settled.claims.empty());};
    auto badSnapshot=snapshot;++badSnapshot.revision;fail(task,badSnapshot,batch,balances);
    badSnapshot=snapshot;badSnapshot.context.boot="old_boot";fail(task,badSnapshot,batch,balances);
    badSnapshot=snapshot;badSnapshot.unresolvedOperation=true;fail(task,badSnapshot,batch,balances);
    badSnapshot=snapshot;badSnapshot.attempts.clear();fail(task,badSnapshot,batch,balances);
    badSnapshot=snapshot;badSnapshot.attempts[0].skillAfter=1;fail(task,badSnapshot,batch,balances);
    badSnapshot=snapshot;badSnapshot.attempts[0].committed=false;fail(task,badSnapshot,batch,balances);
    auto badTask=task;badTask.mode=Mode::Observe;fail(badTask,snapshot,batch,balances);
    for (auto phase : {Phase::Preparing,Phase::Executing,Phase::Reconciling,Phase::Completed}) {
        badTask=task;badTask.phase=phase;fail(badTask,snapshot,batch,balances);
    }
    auto badBatch=batch;badBatch.bookRevision=0;fail(task,snapshot,badBatch,balances);
    badBatch=batch;badBatch.claims.push_back(badBatch.claims.front());fail(task,snapshot,badBatch,balances);
    badBatch=batch;badBatch.claims[0].quantity=5;fail(task,snapshot,badBatch,balances);
    badBatch=batch;badBatch.claims[0].task=receipt;fail(task,snapshot,badBatch,balances);
    badBatch=batch;badBatch.claims[0].actor=704;fail(task,snapshot,badBatch,balances);
    for (const auto& state : {"in_transfer","reconciling","consumed","released"}) {
        badBatch=batch;badBatch.claims[0].state=state;fail(task,snapshot,badBatch,balances);
    }
    badBatch=batch;badBatch.claims[0].nativeReference=99;fail(task,snapshot,badBatch,balances);
    auto wrongBalances=balances;wrongBalances[0].quantity=3;fail(task,snapshot,batch,wrongBalances);
    wrongBalances=balances;wrongBalances[0].location="bank";fail(task,snapshot,batch,wrongBalances);
    wrongBalances=balances;wrongBalances[0].actor=704;fail(task,snapshot,batch,wrongBalances);
    fail(task,snapshot,batch,{});
    for (auto purpose : {ProfessionPurpose::Intermediate,ProfessionPurpose::RequestedItem,ProfessionPurpose::Equipment}) {
        auto promised=job;promised.purpose=purpose;promised.targetSkill=0;badTask=task;
        badTask.checkpoint.data=EncodeProfessionJob(promised);fail(badTask,snapshot,batch,balances);
        assert(blocker=="profession_output_handoff_validator_required");
    }
    assert(book.InstallReceipt(completed.claims)==ClaimInstall::Installed);
    assert(book.InstallReceipt(completed.claims)==ClaimInstall::Duplicate);
    assert(!book.Protection().ProtectedItem(703,102,3371));
    assert(book.Inspect(surplus.id)->quantity==4 && after.stacks[0].count==4); // Goods remain owned.
    assert(book.ReadUnsettled(task.id,batch,blocker) && batch.complete && batch.claims.empty());
    assert(prepare(task,snapshot,batch,{}) && settled.task.phase==Phase::Completed);
    ResourceClaimBook many;std::vector<ResourceClaim> proposals;
    for (unsigned i=0;i<17;++i) {
        auto p=output;p.id="ff2efbdf-f0ec-4539-b840-2998479730"+(i<10 ? std::string("0") : "")+std::to_string(i);
        p.state="proposed";p.itemGuid=0;proposals.push_back(p);
    }
    assert(many.RestoreBatch(proposals)==ClaimInstall::Installed && many.FinishRestore());
    assert(many.ReadUnsettled(task.id,batch,blocker) && !batch.complete && batch.claims.size()==16);
    assert(prepare(task,snapshot,batch,{}) && settled.task.phase==Phase::Verifying && settled.claims.size()==16);
    assert(settled.task.checkpoint.blocker=="profession_claim_settlement_pending");
    assert(many.InstallReceipt(settled.claims)==ClaimInstall::Installed);
    task=settled.task;snapshot.revision=task.revision;
    assert(many.ReadUnsettled(task.id,batch,blocker) && batch.complete && batch.claims.size()==1);
    assert(prepare(task,snapshot,batch,{}) && settled.task.phase==Phase::Completed);
}
