#pragma once
#include "LivingCommissionTradeContract.h"
inline void TestCommissionTradeOperation() {
    ProfessionJob recipe;recipe.recipe=2329;recipe.skill=171;recipe.initialSkill=75;
    recipe.purpose=ProfessionPurpose::RequestedItem;recipe.outputEntry=2454;recipe.outputQuantity=1;
    recipe.reagents={{765,1},{2449,1},{3371,1}};
    CommissionJob job;job.agreement={"lwc-124","wow-tx-direct-offer","direct",EncodeProfessionJob(recipe),703,9,120,1000};
    job.craft=job.agreement.recipe;job.craftFinishedRevision=5;
    Task task;task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f92";task.actor=task.context.actor=703;
    task.source="commission_job";task.sourceKey=job.agreement.id;task.kind=Kind::Commission;
    task.mode=Mode::Active;task.phase=Phase::Preparing;task.revision=5;task.checkpoint.data=EncodeCommissionJob(job);
    task.createdAtMs=task.updatedAtMs=1000;task.accepted=true;
    task.context.boot="ff2efbdf-f0ec-4539-b840-299847970c00";
    task.context.actorGeneration=task.context.mapGeneration=task.context.policyRevision=1;
    const CommissionTradeQuote quote{703,9,2454,120,100,500,{{103,1}}};
    ResourceClaim item;item.id="d1879146-6e96-4e71-827d-6d12d965edb7";item.task=task.id;item.actor=703;
    item.itemGuid=103;item.itemEntry=2454;item.quantity=1;item.state="held";item.location="bags";
    OperationRequest request;request.transition.task=task;request.transition.task.phase=Phase::Executing;
    request.transition.task.checkpoint.step="commission_trade";++request.transition.task.revision;
    request.transition.expectedRevision=task.revision;request.transition.receipt="ff2efbdf-f0ec-4539-b840-299847970c01";
    request.kind="commission_trade";request.effects=Mask(Effect::Inventory)|Mask(Effect::Money);
    request.persistence=NativePersistence::Inventory;request.beforeState=EncodeCommissionTradeQuote(quote);
    request.consumption={{item,1}};auto& action=request.authorization;
    action.task=action.rootTask=task.id;action.world=task.context;action.revision=task.revision;
    action.ownerGeneration=7;action.origin="commission_trade";action.permittedEffects=request.effects;
    std::string why;TestAdapter adapter(request);adapter.consumes=true;
    assert(!ValidateOperationAdapter(request,adapter,why) && why=="native_commission_trade_adapter_required");
    adapter.trade=true;assert(ValidateOperationAdapter(request,adapter,why));
    assert(ValidateOperationRequest(request,task,task.context,nullptr,1000,why));
    assert(OperationRequestWrite(request).receiptQuery.find(SqlValue("commission_trade"))!=std::string::npos);
    assert(VerifyConsumedNativeResources(request,{{703,103,2454,1,0,"bags"}},{},why));
    assert(!VerifyConsumedNativeResources(request,{{703,103,2454,1,0,"bags"}},{{703,103,2454,1,0,"bags"}},why));
    for(unsigned i=0;i<10;++i) {
        auto changed=request;
        if(i==0)changed.beforeState="{}";
        if(i==1)changed.effects=Mask(Effect::Inventory);
        if(i==2)changed.itemGain={2454,1};
        if(i==3)changed.consumption.clear();
        if(i==4)changed.consumption.front().before.location="bank";
        if(i==5)changed.transition.task.checkpoint.step="other_step";
        if(i==6)changed.persistence=NativePersistence::JournalOnly;
        if(i==7){auto q=quote;++q.fee;changed.beforeState=EncodeCommissionTradeQuote(q);}
        if(i==8)changed.transition.task.accepted=false;
        if(i==9)changed.itemTransfer=item;
        TestAdapter bad(changed);bad.consumes=true;bad.trade=true;
        assert(!ValidateOperationAdapter(changed,bad,why));
        assert(!ValidateOperationRequest(changed,task,task.context,nullptr,1000,why));
    }
    adapter.cast=true;assert(!ValidateOperationAdapter(request,adapter,why));
    auto changed=request;changed.kind="vendor_purchase";TestAdapter impostor(changed);impostor.consumes=true;impostor.trade=true;
    assert(!ValidateOperationAdapter(changed,impostor,why) && why=="native_commission_trade_adapter_mismatch");
}
