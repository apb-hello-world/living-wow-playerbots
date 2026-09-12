#pragma once
#include "CraftEvidence.h"
#include "LivingEnchantIntent.h"
namespace LivingActivityTest {
    struct EnchantWorkflowFixture {
        Task task;
        ProfessionJob job;
        CraftFrame before,after;
        EnchantIntent intent;
        EnchantSubject targetAfter;
        ResourceClaim material;
        std::vector<ClaimConsumption> inputs;
        StoredCraftOperation row;
        EnchantWorkflowFixture() {
            task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49106";task.actor=task.context.actor=706;
            task.source="profession_job";task.sourceKey="enchant_workflow";task.kind=Kind::Profession;
            task.mode=Mode::Active;task.phase=Phase::Verifying;task.revision=5;task.createdAtMs=task.updatedAtMs=1000;
            task.context.boot="ff2efbdf-f0ec-4539-b840-299847974106";
            task.context.actorGeneration=2;task.context.mapGeneration=3;task.context.policyRevision=1;
            job.recipe=7443;job.skill=333;job.subjectItem=906;job.initialSkill=20;job.targetSkill=21;
            job.operation=ProfessionOperation::EnchantItem;job.reagents={{10938,1}};task.checkpoint.data=EncodeProfessionJob(job);
            before={706,20,400,{{706,806,10938,1,0,23}}};after={706,21,400,{}};
            material.id="ff2efbdf-f0ec-4539-b840-299847973106";material.actor=706;material.task=task.id;
            material.itemGuid=806;material.itemEntry=10938;material.quantity=1;material.state="held";material.location="bags";
            inputs={{material,1}};
            intent.skill=20;intent.money=400;intent.spec={24,0};intent.claim=material;
            intent.claim.id="ff2efbdf-f0ec-4539-b840-299847975106";intent.claim.itemGuid=906;intent.claim.itemEntry=2398;
            intent.claim.location="equipment";intent.before.item={706,906,2398,1,0,4};intent.before.enchantments.resize(11);
            targetAfter=intent.before;targetAfter.enchantments[0]={24,0,0};
            row.acknowledged=true;row.receipt.id="ff2efbdf-f0ec-4539-b840-299847976106";
            row.receipt.task=task.id;row.receipt.taskRevision=4;row.receipt.kind="profession_craft";
            row.receipt.nativeReference="spell:7443:operation:"+row.receipt.id;
            row.receipt.state=OperationState::Verified;row.receipt.evidence="native_enchant_consumption_subject_and_skill_observed";
            row.beforeState="{\"effects\":21,\"persistence\":2,\"native\":"+ClaimedNativeState(EncodeEnchantIntent(job,intent),inputs)+'}';
            row.afterState=ClaimedNativeState(Capture(false),inputs,8192);
        }
        std::string Capture(bool cancelled) const {
            return "{\"recipe\":7443,\"skill_id\":333,\"effect_entered\":"+std::string(cancelled?"false":"true")+
                ",\"native_finished\":true,\"native_succeeded\":"+(cancelled?"false":"true")+
                ",\"created_calls\":0,\"created_quantity\":0,\"before\":"+FrameJson(before)+",\"after\":"+FrameJson(cancelled?before:after)+
                ",\"enchantment\":24,\"subject_claim\":"+EnchantClaimJson(intent.claim)+",\"subject_before\":"+EnchantSubjectJson(intent.before)+
                ",\"subject_after\":"+EnchantSubjectJson(cancelled?intent.before:targetAfter)+'}';
        }
        UnsettledClaimBatch Held(bool reagent=true) const {
            UnsettledClaimBatch result;result.bookRevision=1;result.complete=true;
            result.claims={intent.claim};if (reagent) result.claims.push_back(material);return result;
        }
        ProfessionSnapshot Snapshot() const {
            ProfessionSnapshot view;view.task=task.id;view.revision=task.revision;view.context=task.context;
            view.complete=view.safe=view.retryReady=view.knownRecipe=view.useful=view.capacity=view.tools=view.atStation=true;
            view.unresolvedOperation=false;view.skill=20;view.stock={{10938,1}};return view;
        }
        StoredCraftOperation Pending() const {
            auto pending=row;pending.receipt.state=OperationState::Intent;pending.receipt.evidence.clear();
            pending.receipt.nativeReference.clear();pending.afterState="{}";return pending;
        }
        StoredCraftOperation MissingCallback() const {
            auto row=Pending();row.receipt.state=OperationState::Reconciling;
            row.receipt.evidence="native_save_capture_requires_reconciliation";
            row.receipt.nativeReference="spell:7443:operation:"+row.receipt.id;
            EnchantCodec::Tree p;std::istringstream in(Capture(false));boost::property_tree::read_json(in,p);
            p.put("effect_entered",false);p.put("native_finished",false);p.put("native_succeeded",false);
            EnchantCodec::Tree frame,target;
            std::istringstream emptyFrame("{\"skill\":0,\"money\":0,\"stacks\":[]}");boost::property_tree::read_json(emptyFrame,frame);
            std::istringstream emptyTarget(EnchantSubjectJson({}));boost::property_tree::read_json(emptyTarget,target);
            p.put_child("after",frame);p.put_child("subject_after",target);row.afterState=EnchantCodec::Json(p);return row;
        }
    };
}
