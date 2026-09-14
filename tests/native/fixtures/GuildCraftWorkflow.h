#pragma once
#include "LivingGuildProcurementHandoff.h"
#include "LivingGuildProcurementRecovery.h"
#include "LivingTaskItemRequirements.h"

namespace LivingActivityTest {
inline void TestGuildCraftWorkflow() {
    GuildProcurementJob guild{4,703,2454,1,"requested_potion","26f357ab-3118-4ab9-a38d-23397d906824"};
    ProfessionJob recipe;recipe.recipe=2329;recipe.skill=171;recipe.initialSkill=75;
    recipe.purpose=ProfessionPurpose::RequestedItem;recipe.outputEntry=2454;recipe.outputQuantity=1;
    recipe.reagents={{765,1},{2449,1},{3371,1}};
    Task task;task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f92";task.actor=task.context.actor=703;
    task.source="guild_procurement";task.sourceKey=GuildProcurementSourceKey(guild);task.kind=Kind::GuildProcurement;
    task.mode=Mode::Active;task.priority=Priority::Delivery;task.phase=Phase::Verifying;task.revision=5;
    task.context.boot="40fdd3bf-585c-4409-9c33-72da5e77eef2";task.context.actorGeneration=2;task.context.mapGeneration=3;
    task.createdAtMs=task.updatedAtMs=1000;task.checkpoint.data=EncodeGuildProcurementJob(guild);
    std::string why;std::vector<ProfessionReagent> need;
    assert(!IsProfessionJob(task) && ReadTaskItemRequirements(task,need,why) && need==std::vector<ProfessionReagent>({{2454,1}}));
    guild.craft=EncodeProfessionJob(recipe);task.checkpoint.data=EncodeGuildProcurementJob(guild);
    assert(ValidateGuildProcurementTask(task,why) && ValidateProfessionTask(task,why) && IsProfessionJob(task));
    assert(ReadTaskItemRequirements(task,need,why) && need==recipe.reagents);
    ProfessionWorkflow flow;assert(DecodeProfessionWorkflow(task.checkpoint.data,flow,why));
    assert(EncodeTaskProfessionWorkflow(task,flow)==task.checkpoint.data);
    GuildProcurementJob decoded;assert(DecodeGuildProcurementJob(task.checkpoint.data,decoded,why));
    assert(decoded.craft==guild.craft && decoded.quantity==1 && !decoded.craftFinishedRevision);
    for(int i=0;i!=6;++i) {
        auto changed=task;auto link=guild;
        if(i==0)link.goal="different_request";
        if(i==1)link.quantity=2;
        if(i==2)link.craft.clear();
        if(i==3){auto job=recipe;job.recipe=2330;link.craft=EncodeProfessionJob(job);}
        if(i==4)link.craftFinishedRevision=task.revision;
        if(i==5)link.guild=5;
        if(!ValidGuildProcurementJob(link))continue;
        changed.checkpoint.data=EncodeGuildProcurementJob(link);++changed.revision;
        assert(!PreserveGuildProcurementIntent(task,changed,why));
    }
    const auto encoded=task.checkpoint.data;
    // Reject recursive envelopes, duplicate fields and forged skill-gain links.
    auto malformed=encoded;const auto nested=malformed.find("profession_job_v1");assert(nested!=std::string::npos);
    malformed.replace(nested,17,"guild_procurement_v2");assert(!DecodeGuildProcurementJob(malformed,decoded,why));
    malformed=encoded;malformed.insert(1,"\"guild\":4,");assert(!DecodeGuildProcurementJob(malformed,decoded,why));
    auto nonRequested=guild;auto wrongRecipe=recipe;wrongRecipe.purpose=ProfessionPurpose::SkillGain;wrongRecipe.targetSkill=76;
    nonRequested.craft=EncodeProfessionJob(wrongRecipe);assert(!ValidGuildProcurementJob(nonRequested));

    CraftFrame before;before.actor=703;before.money=100;before.skill=75;
    before.stacks={{703,100,765,1,0,23},{703,101,2449,1,0,24},{703,102,3371,5,0,25}};
    auto after=before;after.stacks={{703,102,3371,4,0,25},{703,103,2454,1,0,23}}; // No skill gain: still a real requested potion.
    auto evidence=SavedCraft(task,recipe,before,after,false,true);evidence.journalDigest=std::string(64,'a');
    StoredCraftProof proof;assert(DecodeStoredCraftProof(task,evidence,proof,why));
    ProfessionSnapshot snapshot;snapshot.task=task.id;snapshot.revision=task.revision;snapshot.context=task.context;
    snapshot.complete=true;snapshot.unresolvedOperation=false;snapshot.attempts={proof.attempt};snapshot.stock={{765},{2449},{3371}};
    ResourceClaim output;output.id=ItemGainClaimId(proof.attempt.receipt.id,103);output.task=task.id;output.actor=703;
    output.itemGuid=103;output.itemEntry=2454;output.quantity=1;output.state="held";output.location="bags";
    auto spare=output;spare.id="f90f9bb6-58c8-4d82-a3be-a25d3fec47b7";spare.itemGuid=102;spare.itemEntry=3371;spare.quantity=4;
    UnsettledClaimBatch batch;batch.complete=true;batch.bookRevision=1;batch.claims={output,spare};
    std::vector<NativeResourceBalance> balances={{703,103,2454,1,0,"bags"},{703,102,3371,4,0,"bags"}};
    const std::string receipt="d1879146-6e96-4e71-827d-6d12d965edb7";
    ProfessionSettlement result;
    auto prepare=[&] {return PrepareProfessionSettlement(task,snapshot,batch,balances,1001,receipt,result,why);};
    assert(prepare());const auto good=result;
    assert(result.task.id==task.id && result.task.source=="guild_procurement" && result.task.phase==Phase::Preparing);
    assert(result.task.checkpoint.step=="guild_procurement_craft_ready" && result.claims.size()==1);
    assert(result.claims[0].after.id==spare.id && result.claims[0].after.state=="released");
    assert(DecodeGuildProcurementJob(result.task.checkpoint.data,decoded,why) && decoded.craftFinishedRevision==6);
    assert(result.plan.statements[1].find("SHA2(CONCAT(o.before_state")!=std::string::npos);
    assert(result.plan.statements[1].find("i.count=1")!=std::string::npos);
    for(const auto& sql:result.plan.statements)assert(sql.find("INSERT INTO guild_society_supply_delivery")==std::string::npos);
    for(int i=0;i!=9;++i) {
        auto oldSnapshot=snapshot;auto oldBatch=batch;auto oldBalances=balances;
        if(i==0)snapshot.attempts.clear();
        if(i==1)snapshot.attempts[0].journalDigest.clear();
        if(i==2)snapshot.attempts[0].nativeEffectVerified=false;
        if(i==3)batch.claims[0].id=receipt;
        if(i==4)batch.claims[0].quantity=2;
        if(i==5)balances[0].location="bank";
        if(i==6)batch.complete=false;
        if(i==7)batch.claims[1].location="mail";
        if(i==8)snapshot.unresolvedOperation=true;
        assert(!prepare() && !why.empty() && result.plan.statements.empty());
        snapshot=oldSnapshot;batch=oldBatch;balances=oldBalances;
    }
    auto current=task.context;current.boot="1f6c3b70-d58f-4985-a67b-88eb30f6f943";current.policyRevision=2;
    auto fresh=snapshot;fresh.context=current;
    assert(PrepareProfessionRestartSettlement(task,current,fresh,batch,balances,1001,receipt,result,why));
    assert(result.task.id==task.id && result.task.phase==Phase::Preparing && result.task.context==current);
    // A post-craft resume/parcel handoff requires the durable craft receipt,
    // not just a completed flag or possession of an unrelated potion.
    batch.claims={output};balances.resize(1);GuildProcurementHandoff handed;
    assert(PrepareGuildProcurementHandoff(good.task,good.task.context,batch,balances,1002,receipt,handed,why));
    assert(handed.parcels.size()==1 && handed.parcels[0].quantity==1);
    assert(handed.plan.statements[3].find("guild_procurement_craft_verified")!=std::string::npos);
    auto early=task;early.phase=Phase::Preparing;
    assert(!PrepareGuildProcurementHandoff(early,early.context,batch,balances,1002,receipt,handed,why));
    assert(why=="guild_procurement_craft_native_handoff_required");
}
}
