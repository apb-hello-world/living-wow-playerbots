#pragma once
#include "CraftEvidence.h"
#include "LivingProfessionSettlement.h"
namespace LivingActivityTest {
struct ToolPreparationFixture {
    Task task;
    ProfessionWorkflow workflow;
    CraftFrame before,after;
    StoredCraftOperation row;
    StoredCraftProof proof;
    ProfessionSnapshot snapshot;
    UnsettledClaimBatch claims;
    std::vector<NativeResourceBalance> balances;
    std::string reason;
    const std::string receipt="ef05544d-5750-4fae-a0b5-0b6c51082447";
    ToolPreparationFixture() {
        task.id=task.root="5380b70f-d288-42ea-8c81-a1e3ed09f897";
        task.actor=task.context.actor=87001;task.source="profession_job";task.sourceKey="tool_preparation_fixture";
        task.kind=Kind::Profession;task.mode=Mode::Active;task.phase=Phase::Verifying;task.revision=6;
        task.createdAtMs=task.updatedAtMs=1000;task.context.boot="fixture_boot";
        auto& main=workflow.intent;main.recipe=7418;main.skill=333;main.operation=ProfessionOperation::EnchantItem;
        main.subjectItem=8700109;main.initialSkill=1;main.targetSkill=2;main.reagents={{10940,1}};
        ProfessionJob rod;rod.recipe=7421;rod.skill=333;rod.purpose=ProfessionPurpose::Intermediate;
        rod.outputEntry=6218;rod.outputQuantity=1;rod.initialSkill=1;rod.attemptLimit=3;
        rod.reagents={{6217,1},{10938,1},{10940,1}};
        workflow.tools.push_back({rod,3,0});task.checkpoint.data=EncodeProfessionWorkflow(workflow);
        before.actor=task.actor;before.skill=1;before.money=100;
        before.stacks={{task.actor,8700101,6217,1,0,23},{task.actor,8700102,10938,1,0,24},{task.actor,8700103,10940,3,0,25}};
        after=before;after.skill=2;after.stacks={{task.actor,8700103,10940,2,0,25},{task.actor,8700104,6218,1,0,26}};
        row=SavedCraft(task,rod,before,after,false,true);
        row.receipt.id="f9d2509d-1149-4a70-b9f8-c9db10ec8d9a";row.receipt.taskRevision=5;
        row.receipt.nativeReference="spell:7421:operation:"+row.receipt.id;
        // This helper's generic IDs are shared with other SQL scenarios. Give
        // this independent fixture its own claim identities before admission.
        assert(DecodeStoredCraftProof(task,row,proof,reason));unsigned ordinal=0;
        for(const auto& input:proof.inputs) {
            const auto replacement="8dc0da30-132f-45c3-b0d2-ea0ad591b81"+std::to_string(++ordinal);
            for(auto* text:{&row.beforeState,&row.afterState})
                for(size_t at=text->find(input.before.id);at!=std::string::npos;at=text->find(input.before.id,at+replacement.size()))
                    text->replace(at,input.before.id.size(),replacement);
        }
        assert(DecodeStoredCraftProof(task,row,proof,reason));
        // Unit-only digest placeholder; the MariaDB scenario uses its actual
        // saved-row digest returned by ProfessionHistoryQuery instead.
        proof.attempt.journalDigest=std::string(64,'a');
        snapshot.task=task.id;snapshot.revision=task.revision;snapshot.context=task.context;
        snapshot.complete=snapshot.safe=snapshot.retryReady=snapshot.knownRecipe=snapshot.useful=true;
        snapshot.capacity=snapshot.tools=snapshot.atStation=true;snapshot.unresolvedOperation=false;
        snapshot.skill=after.skill;snapshot.stock={{6217},{10938},{10940,2}};snapshot.attempts={proof.attempt};
        claims.complete=true;claims.bookRevision=1;
        for(const auto& use:proof.inputs)if(use.before.quantity>use.used) {
            auto held=use.before;held.quantity-=uint32_t(use.used);++held.revision;claims.claims.push_back(held);
        }
        for(const auto& gained:ItemGainClaims(task,row.receipt.id,{6218,1},proof.gains))claims.claims.push_back(gained.after);
        for(const auto& item:after.stacks)balances.push_back({item.actor,item.guid,item.entry,item.count,0,"bags"});
    }
};
inline void TestProfessionToolHandoff() {
    ToolPreparationFixture f;ProfessionSettlement ready;std::string why;
    auto prepare=[&](){return PrepareProfessionSettlement(f.task,f.snapshot,f.claims,f.balances,1100,f.receipt,ready,why);};
    assert(prepare() && ready.task.phase==Phase::Verifying && ready.task.checkpoint.step=="profession_tool_ready");
    assert(ready.claims.empty() && ready.task.id==f.task.id && ready.task.root==f.task.id);
    ProfessionJob job;assert(DecodeProfessionJob(ready.task.checkpoint.data,job,why) && job.recipe==7418);
    assert(ready.plan.statements[1].find("SHA2(CONCAT(o.before_state,'|',o.after_state),256)")!=std::string::npos);
    const auto finishedTool=ready.task;
    auto fail=[&](){assert(!prepare() && !why.empty() && ready.claims.empty() && ready.plan.statements.empty());};
    auto savedClaims=f.claims;f.claims.complete=false;fail();f.claims=savedClaims;
    f.claims.claims.back().state="in_transfer";fail();f.claims=savedClaims;
    f.claims.claims.back().id=f.receipt;fail();f.claims=savedClaims;
    auto savedBalances=f.balances;f.balances.back().quantity=0;fail();f.balances=savedBalances;
    const auto digest=f.snapshot.attempts[0].journalDigest;f.snapshot.attempts[0].journalDigest.clear();fail();
    f.snapshot.attempts[0].journalDigest=digest;f.snapshot.attempts[0].nativeEffectVerified=false;fail();
    f.snapshot.attempts[0].nativeEffectVerified=true;
    f.task=finishedTool;f.snapshot.revision=f.task.revision;f.snapshot.stock={{10940,2}};
    assert(prepare() && ready.task.phase==Phase::Completed && ready.claims.size()==2);
    assert(ready.plan.statements[1].find(SqlValue("$.native.result.after.skill"))!=std::string::npos); // Rod, not an invented enchant.
    assert(ready.plan.statements[1].find(SqlValue("$.native.after.skill"))==std::string::npos);
}
}
