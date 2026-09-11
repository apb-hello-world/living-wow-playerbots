#include "LivingCraftCapture.h"
#include <atomic>
#include <cassert>
#include <thread>
using namespace LivingActivity;
int main() {
    CraftIdentity identity;
    identity.task="137e6854-06ea-5e21-8371-6b40c8c19f4e";
    identity.operation="2c60183b-4a76-41f4-a62e-01d8c65e273d";
    identity.revision=4;identity.ownerGeneration=9;identity.recipe=2329;identity.skill=171;
    identity.world.actor=235;identity.world.policyRevision=1;
    identity.world.actorGeneration=3;identity.world.mapGeneration=1;
    identity.world.boot="37cfba40-31fe-4664-86db-fb2e87036cb8";
    ProfessionJob job;job.recipe=2329;job.skill=171;job.outputEntry=2454;job.outputQuantity=1;
    job.initialSkill=1;job.targetSkill=2;job.reagents={{765,1},{2449,1},{3371,1}};
    const ItemGainSpec output{2454,1};
    CraftFrame before{235,1,500,{{235,100,765,2,0,23},{235,101,2449,1,0,24},
        {235,102,3371,5,200,0},{235,103,2454,2,200,1}}};
    CraftFrame after=before;after.skill=2;after.stacks[0].count=1;
    after.stacks.erase(after.stacks.begin()+1);after.stacks[1].count=4;after.stacks[2].count=3;
    assert(ValidCraftFrame(before) && ValidCraftFrame(after));
    {
        ResourceView view;assert(view.HasUncertainItem(235,765));assert(view.ProtectedItem(100)>5);
        view.ready=true;assert(!view.HasUncertainItem(235,765));assert(!view.ProtectedItem(100));
        auto exact=std::make_shared<ResourceBucket>();exact->items[100]=10;view.buckets[100%256]=exact;
        assert(!view.UnreservedItem(235,100,765,5) && view.ProtectedItem(100)==10);
        auto uncertain=std::make_shared<ResourceBucket>();uncertain->uncertain[{235,765}]=1;
        view.buckets[235%256]=uncertain;assert(view.HasUncertainItem(235,765));
    }
    auto invalid=before;invalid.stacks.push_back(invalid.stacks.front());assert(!ValidCraftFrame(invalid));
    invalid=before;invalid.stacks[1].slot=23;assert(!ValidCraftFrame(invalid));
    invalid=before;invalid.stacks[0].actor=999;assert(!ValidCraftFrame(invalid));
    invalid=before;invalid.stacks[0].count=0;assert(!ValidCraftFrame(invalid));
    invalid=before;invalid.stacks.resize(257);assert(!ValidCraftFrame(invalid));
    CraftCapture capture(identity);
    assert(!capture.ReadFinished());assert(!capture.Created(identity,2454,1));
    assert(!capture.EnterEffect(identity,before));
    auto stale=identity;++stale.revision;assert(!capture.Start(stale,before));
    assert(capture.Start(identity,before));assert(!capture.Start(identity,before));
    assert(!capture.Finish(stale,true,after));
    assert(capture.EnterEffect(identity,before));assert(!capture.EnterEffect(identity,before));
    assert(capture.Created(identity,2454,1));assert(capture.Finish(identity,true,after));
    assert(!capture.Finish(identity,false,before));assert(!capture.Created(identity,2454,1));
    capture.Abandon(); // A destructor cannot overwrite an observed finish.
    const auto observed=*capture.ReadFinished();
    assert(capture.ReadFinished()->nativeSucceeded);
    auto verified=VerifyCraftCapture(identity,job,observed,output);
    assert(verified.result==CraftEvidence::Verified && verified.blocker.empty());
    assert(verified.gains.size()==1 && verified.gains[0].added==1);
    assert(verified.attempt.nativeEffectVerified && !verified.attempt.committed && verified.attempt.receipt.id.empty());
    assert(verified.attempt.skillBefore==1 && verified.attempt.skillAfter==2);
    auto fails=[&](CraftCaptureResult changed,const char* blocker) {
        const auto result=VerifyCraftCapture(identity,job,changed,output);
        assert(result.result==CraftEvidence::Reconciling && result.blocker==blocker);
        assert(!result.attempt.committed && !result.attempt.nativeEffectVerified);
    };
    auto changed=observed;++changed.identity.revision;fails(changed,"native_craft_identity_mismatch");
    changed=observed;++changed.identity.ownerGeneration;fails(changed,"native_craft_identity_mismatch");
    changed=observed;++changed.identity.world.mapGeneration;fails(changed,"native_craft_identity_mismatch");
    changed=observed;changed.nativeFinished=false;fails(changed,"native_craft_completion_proof_missing");
    changed=observed;changed.createdCalls=0;fails(changed,"native_craft_creation_receipt_mismatch");
    changed=observed;changed.createdCalls=2;fails(changed,"native_craft_creation_receipt_mismatch");
    changed=observed;changed.createdQuantity=2;fails(changed,"native_craft_creation_receipt_mismatch");
    changed=observed;changed.createdEntry=3371;fails(changed,"native_craft_creation_receipt_mismatch");
    changed=observed;changed.nativeSucceeded=false;fails(changed,"native_craft_cancelled_after_possible_effect");
    changed=observed;changed.after.money=499;fails(changed,"native_craft_wallet_or_skill_changed_unexpectedly");
    changed=observed;changed.after.stacks[0].count=2;fails(changed,"native_craft_consumption_mismatch");
    changed=observed;changed.after.stacks[0].guid=555;fails(changed,"native_craft_input_identity_changed");
    changed=observed;changed.after.stacks[0].slot=25;fails(changed,"native_craft_input_identity_changed");
    changed=observed;changed.after.stacks[2].guid=101;fails(changed,"native_craft_input_identity_reused_as_output");
    changed=observed;changed.after.stacks[2].count=2;fails(changed,"native_item_gain_quantity_or_identity_mismatch");
    // A native craft can be successful without gaining skill. The profession
    // job's skill-gain criterion remains unmet; the callback cannot invent it.
    changed=observed;changed.after.skill=1;
    verified=VerifyCraftCapture(identity,job,changed,output);
    assert(verified.result==CraftEvidence::Verified && verified.attempt.skillAfter==1 && !verified.attempt.committed);
    // Item creation after wholly consuming a stack may reuse its SLOT, not its GUID.
    changed=observed;changed.before.stacks.pop_back();changed.after.stacks[2]={235,104,2454,1,0,24};
    assert(VerifyCraftCapture(identity,job,changed,output).result==CraftEvidence::Verified);
    {
        CraftCapture cancelled(identity);assert(cancelled.Start(identity,before));
        assert(cancelled.Finish(identity,false,before));
        assert(VerifyCraftCapture(identity,job,*cancelled.ReadFinished(),output).result==CraftEvidence::RejectedWithoutEffect);
    }
    {
        CraftCapture uncertain(identity);assert(uncertain.Start(identity,before));uncertain.Abandon();
        assert(!uncertain.Finish(identity,true,after));
        fails(*uncertain.ReadFinished(),"native_craft_completion_callback_missing");
    }
    {
        CraftCapture moved(identity);assert(moved.Start(identity,before));auto mutated=before;--mutated.stacks[0].count;
        assert(!moved.EnterEffect(identity,mutated));assert(moved.Finish(identity,false,mutated));
        fails(*moved.ReadFinished(),"native_craft_inputs_changed_before_effect");
    }
    {
        CraftCapture repeated(identity);assert(repeated.Start(identity,before));assert(repeated.EnterEffect(identity,before));
        assert(repeated.Created(identity,2454,1));assert(repeated.Created(identity,2454,1));assert(repeated.Finish(identity,true,after));
        fails(*repeated.ReadFinished(),"native_craft_creation_receipt_mismatch");
    }
    {
        CraftCapture race(identity);std::atomic<unsigned> starts{0};
        std::thread first([&]{if(race.Start(identity,before))++starts;});
        std::thread second([&]{if(race.Start(identity,before))++starts;});first.join();second.join();assert(starts==1);
        assert(race.EnterEffect(identity,before));assert(race.Created(identity,2454,1));
        std::atomic<unsigned> finishes{0};
        std::thread one([&]{if(race.Finish(identity,true,after))++finishes;});
        std::thread two([&]{if(race.Finish(identity,true,after))++finishes;});one.join();two.join();assert(finishes==1);
        assert(VerifyCraftCapture(identity,job,*race.ReadFinished(),output).result==CraftEvidence::Verified);
    }
}
