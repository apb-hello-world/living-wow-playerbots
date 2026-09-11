#pragma once
#include "LivingProfessionResume.h"
namespace LivingActivityTest {
struct ResumeFixture {
    LivingActivity::Task task;
    LivingActivity::WorldContext current;
    LivingActivity::ProfessionSnapshot view;
    LivingActivity::UnsettledClaimBatch batch;
    std::vector<LivingActivity::NativeResourceBalance> balances;
    std::string receipt="ff2efbdf-f0ec-4539-b840-299847977003";
    ResumeFixture() {
        using namespace LivingActivity;
        task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f93";
        task.actor=task.context.actor=704;task.source="profession_job";task.sourceKey="resume_fixture";
        task.mode=Mode::Active;task.phase=Phase::Preparing;task.kind=Kind::Profession;task.revision=3;
        task.createdAtMs=1000;task.updatedAtMs=1100;task.accepted=true;
        task.checkpoint.step="profession_prepare";task.checkpoint.blocker="old_context";
        task.retryAtMs=9000;task.checkpoint.activeElapsedMs=500;task.checkpoint.lastProgressAtMs=1050;task.dueAtMs=9500;
        ProfessionJob job;job.recipe=2329;job.skill=171;job.initialSkill=1;job.targetSkill=2;
        job.outputEntry=2454;job.outputQuantity=1;job.reagents={{765,1},{2449,1},{3371,1}};
        task.checkpoint.data=EncodeProfessionJob(job);
        current=task.context;current.boot="ff2efbdf-f0ec-4539-b840-299847977001";
        current.actorGeneration=7;current.mapGeneration=9;current.policyRevision=3;
        view.task=task.id;view.revision=task.revision;view.context=current;
        view.complete=view.safe=view.knownRecipe=view.useful=view.capacity=view.tools=view.atStation=true;
        view.retryReady=false;view.unresolvedOperation=false;view.skill=1;view.outputPerAttempt=1;
        view.stock={{765,0,0,0,1},{2449,1},{3371,5}};
        batch.complete=true;batch.bookRevision=5;
        ResourceClaim mail;mail.id="ff2efbdf-f0ec-4539-b840-299847977002";
        mail.task=task.id;mail.actor=704;mail.itemGuid=110;mail.itemEntry=765;mail.quantity=1;
        mail.state="held";mail.location="mail";mail.nativeReference=902;mail.revision=1;
        batch.claims={mail};balances={{704,110,765,1,0,"mail",902}};
    }
};
}
