#include "LivingProfessionAttempt.h"
namespace {
void TestProfessionAttemptPlan() {
    Task task;task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f92";
    task.actor=task.context.actor=703;task.source="profession_job";task.sourceKey="ordinary_skill_job";
    task.mode=Mode::Active;task.phase=Phase::Preparing;task.kind=Kind::Profession;task.revision=8;
    task.context.boot="fresh";
    ProfessionJob job;job.recipe=2329;job.skill=171;job.initialSkill=1;job.targetSkill=2;
    job.outputEntry=2454;job.outputQuantity=1;job.reagents={{765,1},{2449,1},{3371,1}};
    task.checkpoint.data=EncodeProfessionJob(job);
    CraftFrame frame;frame.actor=703;frame.money=81;frame.skill=1;
    frame.stacks={{703,100,765,1,0,23},{703,101,2449,1,0,24},{703,102,3371,5,0,25}};
    ProfessionSnapshot view;view.task=task.id;view.revision=8;view.context=task.context;
    view.complete=view.safe=view.retryReady=view.knownRecipe=view.useful=view.capacity=view.tools=view.atStation=true;
    view.unresolvedOperation=false;view.skill=1;view.outputPerAttempt=1;view.stock={{765,1},{2449,1},{3371,5}};
    UnsettledClaimBatch claims;claims.complete=true;
    for (const auto& item : frame.stacks) {
        ResourceClaim claim;claim.id="ff2efbdf-f0ec-4539-b840-299847972"+std::to_string(item.guid);
        claim.actor=703;claim.task=task.id;claim.itemGuid=item.guid;claim.itemEntry=item.entry;
        claim.quantity=item.count;claim.location="bags";claim.state="held";claims.claims.push_back(claim);
    }
    ProfessionAttemptPlan result;std::string blocker;
    {
        std::vector<ProfessionMaterialReservation> missing;
        auto plan=[&](const UnsettledClaimBatch& rows,const CraftFrame& native) {
            return PlanProfessionMaterialReservations(task,view,rows,native,missing,blocker);
        };
        assert(plan(claims,frame) && missing.empty());
        auto partial=claims;partial.claims.erase(partial.claims.begin(),partial.claims.begin()+2);
        assert(plan(partial,frame) && missing.size()==2 && missing[0].quantity==1 && missing[1].quantity==1);
        assert(partial.claims[0].quantity==5); // Paid surplus is inherited, not re-reserved.
        partial.claims.clear();assert(plan(partial,frame) && missing.size()==3 && missing[2].quantity==1);
        partial.complete=false;assert(!plan(partial,frame) && missing.empty());
        partial=claims;partial.claims.back().state="reconciling";assert(!plan(partial,frame) && missing.empty());
        partial=claims;partial.claims.back().actor=704;assert(!plan(partial,frame) && missing.empty());
        partial=claims;partial.claims.back().location="bank";assert(!plan(partial,frame) && missing.empty());
        auto mixed=frame;auto extra=mixed.stacks.back();extra.guid=103;extra.slot=26;mixed.stacks.push_back(extra);
        assert(!plan(claims,mixed) && missing.empty());
        auto stale=view;++stale.revision;
        assert(!PlanProfessionMaterialReservations(task,stale,claims,frame,missing,blocker) && missing.empty());
    }
    auto run=[&](const Task& owner,const ProfessionSnapshot& snapshot,const UnsettledClaimBatch& resources,const CraftFrame& native) {
        return PlanProfessionAttempt(owner,snapshot,resources,native,result,blocker);
    };
    assert(run(task,view,claims,frame));
    assert(result.consumption.size()==3 && result.output.entry==2454 && result.output.quantity==1);
    assert(result.consumption.back().before.id==claims.claims.back().id && result.consumption.back().before.quantity==5 &&
        result.consumption.back().used==1); // Preserve the paid bundle's existing claim.
    assert(result.beforeState=="{\"recipe\":2329,\"skill\":1,\"money\":81}");
    auto fail=[&](const Task& owner,const ProfessionSnapshot& snapshot,const UnsettledClaimBatch& resources,const CraftFrame& native) {
        assert(!run(owner,snapshot,resources,native) && !blocker.empty() && result.consumption.empty() && result.beforeState.empty());
    };
    for (auto phase : {Phase::Queued,Phase::Executing,Phase::Verifying,Phase::Completed,Phase::Cancelled,Phase::Reconciling}) {
        auto changed=task;changed.phase=phase;fail(changed,view,claims,frame);
    }
    auto changed=task;changed.mode=Mode::Observe;fail(changed,view,claims,frame);
    changed=task;changed.parent="parent";fail(changed,view,claims,frame);
    changed=task;changed.accepted=false;fail(changed,view,claims,frame);
    auto snapshot=view;++snapshot.revision;fail(task,snapshot,claims,frame);
    snapshot=view;snapshot.context.boot="old";fail(task,snapshot,claims,frame);
    snapshot=view;snapshot.safe=false;fail(task,snapshot,claims,frame);
    snapshot=view;snapshot.unresolvedOperation=true;fail(task,snapshot,claims,frame);
    snapshot=view;snapshot.outputPerAttempt=0;fail(task,snapshot,claims,frame);
    snapshot=view;snapshot.skill=2;fail(task,snapshot,claims,frame);
    auto rows=claims;rows.complete=false;fail(task,view,rows,frame);
    rows=claims;rows.claims.pop_back();fail(task,view,rows,frame);
    rows=claims;rows.claims.push_back(rows.claims.back());fail(task,view,rows,frame);
    for (auto field : {0,1,2,3,4,5,6,7,8}) {
        rows=claims;auto& claim=rows.claims.back();
        if (field==0) ++claim.actor;
        if (field==1) claim.task="elsewhere";
        if (field==2) ++claim.itemGuid;
        if (field==3) claim.location="bank";
        if (field==4) claim.state="reconciling";
        if (field==5) claim.quantity=6;
        if (field==6) claim.quantity=0;
        if (field==7) claim.nativeReference=1;
        if (field==8) claim.copper=1;
        fail(task,view,rows,frame);
    }
    auto native=frame;++native.actor;fail(task,view,claims,native);
    native=frame;native.stacks.pop_back();fail(task,view,claims,native);
    native=frame;native.stacks.back().count=4;fail(task,view,claims,native);
    native=frame;auto extra=native.stacks.back();extra.guid=103;extra.slot=26;native.stacks.push_back(extra);
    fail(task,view,claims,native); // Native consumption order needs explicit safe split preparation.
    assert(run(task,view,claims,frame));
    assert(claims.claims.back().quantity==5 && frame.money==81 && task.phase==Phase::Preparing);
}
}
