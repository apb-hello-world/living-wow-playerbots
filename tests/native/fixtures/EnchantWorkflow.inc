#include "EnchantWorkflow.h"
#include "LivingProfessionAttempt.h"
#include "LivingProfessionResume.h"
namespace {
    void TestEnchantWorkflow() {
        using namespace LivingActivityTest;using namespace LivingActivity;
        EnchantWorkflowFixture f;std::string blocker;StoredCraftProof proof;
        assert(DecodeStoredCraftProof(f.task,f.row,proof,blocker));
        assert(proof.attempt.committed && proof.attempt.nativeEffectVerified && proof.attempt.subjectItem==906 && proof.gains.empty());
        auto cancelled=f.row;cancelled.receipt.state=OperationState::Rejected;
        cancelled.receipt.evidence="native_cast_cancelled_without_effect";cancelled.afterState=f.Capture(true);
        assert(DecodeStoredCraftProof(f.task,cancelled,proof,blocker));
        assert(proof.attempt.committed && !proof.attempt.nativeEffectVerified && proof.attempt.subjectItem==906);
        auto mutate=[&](StoredCraftOperation& row,const char* path,uint32_t value) {
            EnchantCodec::Tree p;std::istringstream in(row.afterState);boost::property_tree::read_json(in,p);
            p.put(path,value);row.afterState=EnchantCodec::Json(p);
        };
        for (const auto* field:{"native.enchantment","native.subject_after.guid","native.subject_after.entry",
             "native.subject_before.slot","native.subject_claim.revision","native.after.money","native.created_quantity"}) {
            auto changed=f.row;mutate(changed,field,99);
            assert(!DecodeStoredCraftProof(f.task,changed,proof,blocker) && !proof.attempt.committed && proof.gains.empty());
        }
        EnchantIntent parsed;
        assert(DecodeEnchantIntent(f.task,f.job,EncodeEnchantIntent(f.job,f.intent),parsed,blocker));
        assert(SameResourceClaim(parsed.claim,f.intent.claim) && SameEnchantSubject(parsed.before,f.intent.before));
        auto intent=f.intent;intent.claim.quantity=2;
        assert(!ValidResourceClaim(intent.claim)); // Equipment can never be a spendable stack.
        intent=f.intent;intent.claim.location="bags";
        assert(!DecodeEnchantIntent(f.task,f.job,EncodeEnchantIntent(f.job,intent),parsed,blocker));
        intent.before.item.slot=24;
        assert(DecodeEnchantIntent(f.task,f.job,EncodeEnchantIntent(f.job,intent),parsed,blocker));
        auto task=f.task;task.phase=Phase::Preparing;auto snapshot=f.Snapshot();ProfessionAttemptPlan plan;
        assert(PlanProfessionAttempt(task,snapshot,f.Held(),f.before,plan,blocker));
        assert(plan.output.Empty() && plan.consumption.size()==1 && plan.consumption[0].before.itemGuid==806);
        auto missing=f.Held();missing.claims.erase(missing.claims.begin());
        assert(!PlanProfessionAttempt(task,snapshot,missing,f.before,plan,blocker));
        assert(blocker=="native_enchant_subject_reservation_required");
        missing=f.Held();missing.claims[0].state="released";
        assert(!PlanProfessionAttempt(task,snapshot,missing,f.before,plan,blocker));
        assert(DecodeStoredCraftProof(f.task,f.row,proof,blocker));snapshot.attempts={proof.attempt};snapshot.skill=21;
        const std::vector<NativeResourceBalance> balances={{706,906,2398,1,0,"equipment"}};
        ProfessionSettlement settled;const auto receipt="ff2efbdf-f0ec-4539-b840-299847977106";
        assert(PrepareProfessionSettlement(f.task,snapshot,f.Held(false),balances,1200,receipt,settled,blocker));
        assert(settled.task.phase==Phase::Completed && settled.claims.size()==1 && settled.claims[0].after.state=="released");
        assert(settled.plan.statements[1].find(SqlValue("$.native.after.skill"))!=std::string::npos);
        assert(settled.plan.statements[1].find(SqlValue("native_enchant_consumption_subject_and_skill_observed"))!=std::string::npos);
        snapshot.attempts[0].skillAfter=20;
        assert(!PrepareProfessionSettlement(f.task,snapshot,f.Held(false),balances,1200,receipt,settled,blocker));
        // Crash before native transaction commits: original subject, every
        // reagent and every exact hold must still exist unchanged.
        task=f.task;task.phase=Phase::Executing;task.revision=4;task.checkpoint.step="profession_craft";
        task.context.boot.clear();task.context.actorGeneration=task.context.mapGeneration=0;
        auto current=f.task.context;current.boot="ff2efbdf-f0ec-4539-b840-299847978106";
        ProfessionHistory history;history.task=task.id;history.revision=4;history.complete=history.unresolvedOperation=true;
        history.interruptedCraft=f.Pending();ProfessionPreparation resumed;
        auto restore=[&](const EnchantSubject& subject,const UnsettledClaimBatch& claims,const CraftFrame& frame) {
            return PrepareInterruptedProfession(task,current,history,claims,frame,1200,receipt,resumed,blocker,{},&subject);
        };
        assert(restore(f.intent.before,f.Held(),f.before));
        assert(resumed.plan.statements[1].find("i.enchantments=")!=std::string::npos);
        auto target=f.intent.before;target.enchantments[0].id=24;assert(!restore(target,f.Held(),f.before));
        target=f.intent.before;target.item.slot=24;assert(!restore(target,f.Held(),f.before));
        missing=f.Held();missing.claims[0].revision++;assert(!restore(f.intent.before,missing,f.before));
        auto frame=f.before;frame.stacks.clear();assert(!restore(f.intent.before,f.Held(),frame));
        assert(restore(f.intent.before,f.Held(),f.before));
        auto recovered=f.Pending();recovered.receipt.state=OperationState::Rejected;
        recovered.receipt.evidence="native_craft_intent_not_committed";
        recovered.receipt.nativeReference=f.row.receipt.nativeReference;
        recovered.afterState="{\"recovery\":{\"version\":1,\"basis\":\"atomic_native_save_absent\",\"boot\":\""+
            current.boot+"\"},\"frame\":"+FrameJson(f.before)+",\"subject\":"+EnchantSubjectJson(f.intent.before)+'}';
        assert(DecodeStoredCraftProof(resumed.task,recovered,proof,blocker));
        assert(proof.attempt.committed && !proof.attempt.nativeEffectVerified && proof.attempt.subjectItem==906);
        mutate(recovered,"subject.guid",907);assert(!DecodeStoredCraftProof(resumed.task,recovered,proof,blocker));
        auto uncertain=f.Pending();uncertain.receipt.state=OperationState::Reconciling;
        InterruptedCraftIntent interrupted;assert(!DecodeInterruptedCraftIntent(task,uncertain,interrupted,blocker));
        // Observed native callback gap, not arbitrary uncertainty. A restart
        // compares exact untouched subject/material/skill/money before retry.
        uncertain=f.MissingCallback();task.phase=Phase::Reconciling;++task.revision;
        task.checkpoint.blocker=uncertain.receipt.evidence;
        assert(DecodeInterruptedCraftIntent(task,uncertain,interrupted,blocker));
        history.revision=task.revision;history.interruptedCraft=uncertain;
        assert(restore(f.intent.before,f.Held(),f.before));
        assert(resumed.plan.statements[1].find("o.state="+SqlValue("reconciling"))!=std::string::npos);
        assert(resumed.plan.statements[1].find("o.after_state="+SqlValue(uncertain.afterState))!=std::string::npos);
        recovered=f.Pending();recovered.receipt.state=OperationState::Rejected;
        recovered.receipt.evidence="native_craft_intent_not_committed";recovered.receipt.nativeReference=f.row.receipt.nativeReference;
        recovered.afterState="{\"recovery\":{\"version\":1,\"basis\":\"atomic_native_save_absent\",\"boot\":\""+
            current.boot+"\"},\"frame\":"+FrameJson(f.before)+",\"subject\":"+EnchantSubjectJson(f.intent.before)+
            ",\"prior_observation\":"+uncertain.afterState+'}';
        assert(DecodeStoredCraftProof(resumed.task,recovered,proof,blocker));
        target=f.intent.before;target.enchantments[0].id=24;assert(!restore(target,f.Held(),f.before));
        frame=f.before;frame.stacks.clear();assert(!restore(f.intent.before,f.Held(),frame));
        mutate(uncertain,"effect_entered",1);assert(!DecodeInterruptedCraftIntent(task,uncertain,interrupted,blocker));
        uncertain=f.MissingCallback();mutate(uncertain,"after.skill",20);
        assert(!DecodeInterruptedCraftIntent(task,uncertain,interrupted,blocker));
    }
}
