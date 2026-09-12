#pragma once
#include "ProfessionResume.h"
#include "CraftEvidence.h"
namespace LivingActivityTest {
struct InterruptedFixture {
    Task task;
    WorldContext current;
    ProfessionHistory history;
    UnsettledClaimBatch batch;
    CraftFrame frame;
    std::vector<NativeItemStack> bank;
    std::string receipt="ff2efbdf-f0ec-4539-b840-299847978003";
    InterruptedFixture() {
        ResumeFixture base;task=base.task;task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f94";
        task.sourceKey="interrupted_fixture";task.actor=task.context.actor=705;
        task.phase=Phase::Executing;task.revision=4;task.checkpoint.step="profession_craft";
        current=base.current;current.actor=705;
        ProfessionJob job;std::string blocker;assert(DecodeProfessionJob(task.checkpoint.data,job,blocker));
        frame={705,1,100,{{705,800,765,1,0,23},{705,801,2449,1,0,24},{705,802,3371,5,0,25}}};
        auto row=SavedCraft(task,job,frame,frame,true,true);
        row.receipt.id="ff2efbdf-f0ec-4539-b840-299847978004";
        row.receipt.state=OperationState::Intent;row.receipt.nativeReference.clear();row.receipt.evidence.clear();row.afterState="{}";
        InterruptedCraftIntent intent;assert(DecodeInterruptedCraftIntent(task,row,intent,blocker));
        unsigned index=0;
        for(auto& use:intent.inputs) {
            const auto old=use.before.id;use.before.id="b71b6cac-8b42-44d9-9f1f-24e4e40a000"+std::to_string(++index);
            const auto pos=row.beforeState.find(old);assert(pos!=std::string::npos);
            row.beforeState.replace(pos,old.size(),use.before.id);
        }
        history.task=task.id;history.revision=task.revision;history.complete=history.unresolvedOperation=true;
        history.interruptedCraft=row;
        batch.complete=true;batch.bookRevision=1;
        for(const auto& use:intent.inputs) batch.claims.push_back(use.before);
    }
    void AddBankDependency() {
        auto claim=batch.claims.front();claim.id="b71b6cac-8b42-44d9-9f1f-24e4e40a0004";
        claim.itemGuid=803;claim.itemEntry=4470;claim.quantity=2;claim.location="bank";claim.revision=1;
        batch.claims.push_back(claim);bank.push_back({705,803,4470,2,0,43});
    }
};
}
