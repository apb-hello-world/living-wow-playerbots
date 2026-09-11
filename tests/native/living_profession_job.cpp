#include "LivingProfessionJob.h"
#include "LivingActivityRequests.h"
#include <cassert>
using namespace LivingActivity;
int main() {
    ProfessionJob job; job.recipe=2881; job.skill=165; job.outputEntry=2318; job.outputQuantity=1;
    job.initialSkill=1; job.targetSkill=2; job.reagents={{2934,3}};
    std::string reason;
    assert(ValidateProfessionJob(job,reason));
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
}
