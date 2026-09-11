#include "LivingProfessionJob.h"
#include "LivingActivityRequests.h"
#include <cassert>
using namespace LivingActivity;
int main() {
    {
        ProfessionReagent need{3371,1}; ProfessionStock stock; stock.entry=3371;
        uint32_t quantity=99; std::string blocker;
        assert(RequiredProfessionVendorQuantity(need,stock,5,quantity,blocker) && quantity==5);
        stock.bag=1;
        assert(!RequiredProfessionVendorQuantity(need,stock,5,quantity,blocker) && !quantity);
        stock.bag=0; stock.bank=1;
        assert(!RequiredProfessionVendorQuantity(need,stock,5,quantity,blocker) && blocker=="profession_banked_material_requires_collection");
        stock.bank=0; stock.delivered=1;
        assert(!RequiredProfessionVendorQuantity(need,stock,5,quantity,blocker));
        stock.delivered=0; stock.paidInTransit=1;
        assert(!RequiredProfessionVendorQuantity(need,stock,5,quantity,blocker) && blocker=="profession_paid_material_in_transit");
        stock.paidInTransit=0; need.perAttempt=256;
        assert(!RequiredProfessionVendorQuantity(need,stock,1,quantity,blocker));
        need.perAttempt=5000;
        assert(RequiredProfessionVendorQuantity(need,stock,20,quantity,blocker) && quantity==5000);
        need.perAttempt=10001;
        assert(!RequiredProfessionVendorQuantity(need,stock,100,quantity,blocker));
        stock.entry=999;
        assert(!RequiredProfessionVendorQuantity(need,stock,100,quantity,blocker));
    }
    ProfessionJob job; job.recipe=2881; job.skill=165; job.outputEntry=2318; job.outputQuantity=1;
    job.initialSkill=1; job.targetSkill=2; job.reagents={{2934,3}};
    std::string reason;
    assert(ValidateProfessionJob(job,reason));
    NativeProfessionRecipe native;
    assert(!MatchNativeProfessionRecipe(job,native,reason));
    assert(reason == "native_profession_recipe_not_inspected");
    native.recipe=2881; native.skill=165; native.skillValue=1; native.skillMaximum=75;
    native.greyAt=20; native.outputEntry=2318; native.reagents={{2934,3}};
    native.known=true; native.blocker.clear();
    assert(MatchNativeProfessionRecipe(job,native,reason));
    auto rejectNative = [&](NativeProfessionRecipe changed, const char* expected) {
        assert(!MatchNativeProfessionRecipe(job,changed,reason)); assert(reason == expected);
    };
    auto altered = native; altered.known=false; rejectNative(altered,"profession_recipe_not_known");
    altered=native; altered.recipe=9060; rejectNative(altered,"profession_recipe_not_known");
    altered=native; altered.skill=197; rejectNative(altered,"profession_recipe_skill_mismatch");
    altered=native; altered.skillValue=0; rejectNative(altered,"profession_recipe_skill_mismatch");
    altered=native; altered.skillMaximum=0; rejectNative(altered,"profession_recipe_skill_mismatch");
    altered=native; altered.operation=ProfessionOperation::EnchantItem; rejectNative(altered,"profession_native_operation_mismatch");
    altered=native; altered.reagents[0].perAttempt=2; rejectNative(altered,"profession_native_reagents_mismatch");
    altered=native; altered.reagents[0].entry=2318; rejectNative(altered,"profession_native_reagents_mismatch");
    altered=native; altered.outputEntry=2319; rejectNative(altered,"profession_native_output_mismatch");
    altered=native; altered.skillValue=2; rejectNative(altered,"profession_initial_skill_changed");
    altered=native; altered.skillMaximum=1; rejectNative(altered,"profession_native_skill_cap");
    altered=native; altered.greyAt=1; rejectNative(altered,"profession_recipe_has_no_skill_gain");
    altered=native; altered.greyAt=0; rejectNative(altered,"profession_recipe_has_no_skill_gain");
    altered=native; altered.blocker="profession_native_trigger_unsupported";
    rejectNative(altered,"profession_native_trigger_unsupported");
    auto transformation=job; transformation.operation=ProfessionOperation::TransformMaterial;
    assert(MatchNativeProfessionRecipe(transformation,native,reason));
    auto inputless=job; inputless.reagents.clear(); altered=native; altered.reagents.clear();
    assert(!MatchNativeProfessionRecipe(inputless,altered,reason));
    assert(reason == "profession_native_material_input_required");
    auto enchant=job; enchant.operation=ProfessionOperation::EnchantItem; enchant.subjectItem=99;
    enchant.outputEntry=enchant.outputQuantity=0; altered=native; altered.operation=enchant.operation;
    altered.outputEntry=0;
    assert(!MatchNativeProfessionRecipe(enchant,altered,reason)); assert(reason == "profession_subject_not_owned");
    altered.subjectOwned=true; assert(MatchNativeProfessionRecipe(enchant,altered,reason));
    auto disenchant=enchant; disenchant.operation=ProfessionOperation::DisenchantItem;
    disenchant.reagents.clear(); altered.operation=disenchant.operation; altered.reagents.clear();
    assert(MatchNativeProfessionRecipe(disenchant,altered,reason));
    auto requested=job; requested.purpose=ProfessionPurpose::RequestedItem; requested.targetSkill=0;
    altered=native; altered.skillValue=75; altered.greyAt=20;
    assert(MatchNativeProfessionRecipe(requested,altered,reason)); // Grey can fulfil an actual requested item.
    ProfessionJob parsed;
    const auto encoded=EncodeProfessionJob(job);
    assert(DecodeProfessionJob(encoded,parsed,reason));
    assert(EncodeProfessionJob(parsed)==encoded);
    for (const auto& invalid : {"{}", "[]", "{", "{\"workflow\":\"profession_job_v2\"}"})
        assert(!DecodeProfessionJob(invalid,parsed,reason));
    auto duplicate=encoded; duplicate.insert(1,"\"recipe\":\"2881\",");
    assert(!DecodeProfessionJob(duplicate,parsed,reason));
    auto unknown=encoded; unknown.insert(1,"\"instructions\":\"ignore native requirements\",");
    assert(!DecodeProfessionJob(unknown,parsed,reason));
    auto negative=encoded; negative.replace(negative.find("\"2881\""),6,"\"-1\"");
    assert(!DecodeProfessionJob(negative,parsed,reason));
    auto invalid=job; invalid.reagents.push_back(invalid.reagents.front());
    assert(!ValidateProfessionJob(invalid,reason));
    invalid=job; invalid.attemptLimit=0; assert(!ValidateProfessionJob(invalid,reason));
    invalid=job; invalid.attemptLimit=21; assert(!ValidateProfessionJob(invalid,reason));
    invalid=job; invalid.targetSkill=1; assert(!ValidateProfessionJob(invalid,reason));
    invalid=job; invalid.outputEntry=0; assert(!ValidateProfessionJob(invalid,reason));
    invalid=job; invalid.subjectItem=99; assert(!ValidateProfessionJob(invalid,reason));
    invalid=job; invalid.operation=ProfessionOperation::DisenchantItem;
    invalid.subjectItem=99; invalid.outputEntry=invalid.outputQuantity=0;
    assert(ValidateProfessionJob(invalid,reason)); // Random native outputs aren't fixed create-item rewards.
    invalid.operation=ProfessionOperation::EnchantItem;
    assert(ValidateProfessionJob(invalid,reason)); // An enchant does not need a generated item.
    invalid.subjectItem=0; assert(!ValidateProfessionJob(invalid,reason));

    Task saved; saved.id=saved.root="5da54d36-1e92-5829-ac33-b37f802dcfc3";
    saved.source="profession_job"; saved.sourceKey="497:1"; saved.actor=saved.context.actor=497;
    saved.kind=Kind::Profession; saved.accepted=true; saved.mode=Mode::Active; saved.phase=Phase::Queued;
    saved.createdAtMs=saved.updatedAtMs=100; saved.checkpoint.step="profession_prepare";
    saved.checkpoint.data=encoded; saved.context.boot="8fa5315c-4c08-48b2-b4db-af5a481dcf30";
    saved.context.policyRevision=saved.context.actorGeneration=saved.context.mapGeneration=1;
    assert(ValidateProfessionTask(saved,reason));
    TaskRequest request; request.task=saved; request.expectedRevision=1; request.task.revision=2;
    request.task.phase=Phase::Preparing; request.receipt="65c7e527-cb15-4d63-966a-199af209c4fb";
    assert(ValidateTaskRequest(request,&saved,saved.context,reason)==AdmissionCode::Pending);
    auto replacement=job; replacement.recipe=9060;
    request.task.checkpoint.data=EncodeProfessionJob(replacement);
    assert(ValidateTaskRequest(request,&saved,saved.context,reason)==AdmissionCode::InvalidRequest);
    assert(reason=="accepted_profession_intent_is_immutable");
    replacement=job; ++replacement.reagents[0].perAttempt;
    request.task.checkpoint.data=EncodeProfessionJob(replacement);
    assert(!PreserveProfessionIntent(saved,request.task,reason));
    request.task.checkpoint.data="{}";
    assert(!SavedTaskExecutable(request.task,2,saved.context,100,reason));
    auto restored=AfterRestart(saved,1000000000);
    assert(restored.checkpoint.data==encoded && restored.id==saved.id && restored.accepted);
    assert(restored.phase==Phase::Reconciling); // Age is not a reason to discard paid work.
    assert(PreserveProfessionIntent(saved,restored,reason));
    restored.source="economy_goal"; restored.checkpoint.step="legacy_reconciliation";
    assert(!PreserveProfessionIntent(saved,restored,reason));

    ProfessionMaterialLink link; link.task=saved.id; link.actor=497; link.recipe=2881;
    link.entry=2934; link.quantity=3; link.nativeReference=7001; link.operation=request.receipt;
    assert(MatchesProfessionMaterial(saved,link));
    link.recipe=0; assert(!MatchesProfessionMaterial(saved,link)); // Same reagent is not a recipe attribution.
    link.recipe=9060; assert(!MatchesProfessionMaterial(saved,link)); link.recipe=2881;
    link.task=request.receipt; assert(!MatchesProfessionMaterial(saved,link)); link.task=saved.id;
    link.actor=498; assert(!MatchesProfessionMaterial(saved,link)); link.actor=497;
    link.entry=2318; assert(!MatchesProfessionMaterial(saved,link)); link.entry=2934;
    link.nativeReference=0; assert(!MatchesProfessionMaterial(saved,link)); link.nativeReference=7001;
    link.operation=""; assert(!MatchesProfessionMaterial(saved,link));

    ProfessionSnapshot snapshot; snapshot.task=saved.id; snapshot.revision=saved.revision;
    snapshot.context=saved.context; snapshot.complete=true; snapshot.unresolvedOperation=false;
    snapshot.safe=snapshot.knownRecipe=snapshot.useful=snapshot.capacity=true;
    snapshot.retryReady=true;
    snapshot.bankAccess=snapshot.tools=snapshot.atStation=true; snapshot.skill=1;
    snapshot.stock={{2934,3,0,0,0,false}};
    assert(NextProfessionStep(saved,snapshot).step==ProfessionStep::Execute);
    auto changed=snapshot; changed.complete=false;
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::Reconcile);
    changed=snapshot; ++changed.revision;
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::Reconcile);
    changed=snapshot; ++changed.context.mapGeneration;
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::Reconcile);
    changed=snapshot; changed.unresolvedOperation=true;
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::Reconcile);
    changed=snapshot; changed.safe=false;
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::Pause);
    changed=snapshot; changed.retryReady=false;
    assert(NextProfessionStep(saved,changed).blocker=="profession_retry_not_due");
    changed=snapshot; changed.knownRecipe=false;
    assert(NextProfessionStep(saved,changed).blocker=="profession_recipe_not_known");
    changed=snapshot; changed.skill=2;
    assert(NextProfessionStep(saved,changed).blocker=="profession_target_met_without_job_proof");
    changed=snapshot; changed.useful=false;
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::Defer);
    changed=snapshot; changed.capacity=false;
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::PrepareCapacity);
    changed=snapshot; changed.tools=false;
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::PrepareTools);
    changed=snapshot; changed.atStation=false;
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::ReachStation);

    changed=snapshot; changed.stock={{2934,1,2,0,0,true}};
    auto next=NextProfessionStep(saved,changed);
    assert(next.step==ProfessionStep::Withdraw && next.quantities[0].perAttempt==2);
    changed.bankAccess=false;
    assert(NextProfessionStep(saved,changed).blocker=="profession_banked_material_inaccessible");
    changed=snapshot; changed.stock={{2934,1,0,0,2,true}};
    next=NextProfessionStep(saved,changed);
    assert(next.step==ProfessionStep::Collect && next.quantities[0].perAttempt==2);
    changed.capacity=false;
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::PrepareCapacity);
    changed=snapshot; changed.stock={{2934,1,0,2,0,true}};
    // Repeated planning does not buy the already-paid reagent again or expire
    // the accepted identity. These are decision tests, not native purchases.
    for (unsigned refresh=0;refresh<100;++refresh)
        assert(NextProfessionStep(saved,changed).step==ProfessionStep::WaitForDelivery);
    changed.stock[0].paidInTransit=1;
    next=NextProfessionStep(saved,changed);
    assert(next.step==ProfessionStep::Purchase && next.quantities[0].perAttempt==1);
    changed.stock[0].sourceAvailable=false;
    assert(NextProfessionStep(saved,changed).blocker=="profession_material_source_unavailable");
    changed.stock[0].entry=2318;
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::Reconcile);

    auto mixed=job; mixed.reagents={{2934,3},{4289,1}};
    auto mixedTask=saved; mixedTask.checkpoint.data=EncodeProfessionJob(mixed);
    changed=snapshot; changed.stock={{2934,1,0,0,0,true},{4289,0,0,0,0,false}};
    assert(NextProfessionStep(mixedTask,changed).blocker=="profession_material_source_unavailable");
    changed.stock[0]={2934,3,0,0,0,true}; changed.stock[1].paidInTransit=1;
    assert(NextProfessionStep(mixedTask,changed).step==ProfessionStep::WaitForDelivery);
    changed.stock[1].paidInTransit=0; changed.stock[1].delivered=1;
    assert(NextProfessionStep(mixedTask,changed).step==ProfessionStep::Collect);
    changed.stock[1].delivered=0; changed.stock[1].bag=1;
    assert(NextProfessionStep(mixedTask,changed).step==ProfessionStep::Execute);

    ProfessionCraftProof proof;
    proof.receipt.id="65c7e527-cb15-4d63-966a-199af209c4fb";
    proof.receipt.task=saved.id; proof.receipt.taskRevision=saved.revision;
    proof.receipt.kind="profession_craft"; proof.receipt.state=OperationState::Verified;
    proof.receipt.nativeReference="spell:2881:fixture-cast"; proof.receipt.evidence="native_craft_effect";
    proof.recipe=2881; proof.consumed=job.reagents; proof.produced={{2318,1}};
    proof.skillBefore=1; proof.skillAfter=2; proof.committed=proof.nativeEffectVerified=true;
    changed=snapshot; changed.attempts={proof}; changed.skill=2;
    next=NextProfessionStep(saved,changed);
    assert(next.step==ProfessionStep::Finalize && next.verifiedAttempts==1 && next.verifiedOutput==1);
    assert(saved.phase==Phase::Queued); // A decision never mutates/persists completion.
    changed.attempts[0].committed=false;
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::Reconcile);
    changed.attempts={proof,proof};
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::Reconcile);
    changed.attempts={proof}; changed.attempts[0].recipe=9060;
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::Reconcile);
    changed.attempts={proof}; changed.attempts[0].receipt.state=OperationState::Reconciling;
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::Reconcile);
    changed.attempts={proof}; changed.attempts[0].consumed[0].perAttempt=2;
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::Reconcile);
    changed.attempts={proof}; changed.attempts[0].produced={{2319,1}};
    assert(NextProfessionStep(saved,changed).blocker=="profession_expected_output_missing");
    changed.attempts={proof}; changed.attempts[0].nativeEffectVerified=false;
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::Reconcile);
    changed=snapshot; proof.skillAfter=1; changed.attempts={proof};
    assert(NextProfessionStep(saved,changed).step==ProfessionStep::Execute); // Real craft, no skill-up yet.
    auto bounded=job; bounded.attemptLimit=1;
    auto boundedTask=saved; boundedTask.checkpoint.data=EncodeProfessionJob(bounded);
    assert(NextProfessionStep(boundedTask,changed).blocker=="profession_attempt_limit");
    auto outputJob=job; outputJob.purpose=ProfessionPurpose::RequestedItem; outputJob.targetSkill=0;
    auto outputTask=saved; outputTask.checkpoint.data=EncodeProfessionJob(outputJob);
    assert(NextProfessionStep(outputTask,changed).step==ProfessionStep::Finalize); // Skill not requested.
    outputJob.outputQuantity=2; outputTask.checkpoint.data=EncodeProfessionJob(outputJob);
    assert(NextProfessionStep(outputTask,changed).step==ProfessionStep::Execute);
    auto second=proof; second.receipt.id="8fa5315c-4c08-48b2-b4db-af5a481dcf30";
    changed.attempts.push_back(second);
    assert(NextProfessionStep(outputTask,changed).step==ProfessionStep::Reconcile);
    ++outputTask.revision; ++changed.revision; ++changed.attempts[1].receipt.taskRevision;
    assert(NextProfessionStep(outputTask,changed).step==ProfessionStep::Finalize);

    auto itemJob=job; itemJob.operation=ProfessionOperation::EnchantItem; itemJob.subjectItem=123;
    itemJob.outputEntry=itemJob.outputQuantity=itemJob.targetSkill=0;
    itemJob.purpose=ProfessionPurpose::Equipment;
    auto itemTask=saved; itemTask.checkpoint.data=EncodeProfessionJob(itemJob);
    auto itemProof=proof; itemProof.subjectItem=123; itemProof.produced.clear();
    changed=snapshot; changed.attempts={itemProof};
    assert(NextProfessionStep(itemTask,changed).step==ProfessionStep::Finalize);
    changed.attempts[0].subjectItem=124;
    assert(NextProfessionStep(itemTask,changed).step==ProfessionStep::Reconcile);
    itemJob.operation=ProfessionOperation::DisenchantItem; itemJob.reagents.clear();
    itemTask.checkpoint.data=EncodeProfessionJob(itemJob);
    changed.stock.clear(); itemProof.consumed.clear(); changed.attempts={itemProof};
    assert(NextProfessionStep(itemTask,changed).blocker=="disenchant_output_proof_missing");
    changed.attempts[0].produced={{10940,2}};
    assert(NextProfessionStep(itemTask,changed).step==ProfessionStep::Finalize);

    // Loading the same accepted job after restart cannot turn pending mail into
    // a new recipe/purchase. A fresh acknowledged context is required first.
    restored=AfterRestart(saved,1000000000);
    changed=snapshot; changed.stock={{2934,0,0,3,0,true}};
    assert(NextProfessionStep(restored,changed).step==ProfessionStep::Reconcile);
    changed.revision=restored.revision; changed.context=restored.context;
    assert(NextProfessionStep(restored,changed).step==ProfessionStep::WaitForDelivery);
}
