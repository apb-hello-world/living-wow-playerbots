#pragma once
#include "LivingCommissionTradeContract.h"
#include "LivingCommissionTradeSettlement.h"
#include "LivingActivityAuthority.h"
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
    {
        auto ready=task;ready.checkpoint.step="commission_craft_ready";
        auto current=task.context;current.boot="c859a150-352b-4685-93c1-a35b7728e495";
        ProfessionHistory history;history.task=ready.id;history.revision=ready.revision;history.complete=true;
        history.unresolvedOperation=false;history.attempts.emplace_back();
        UnsettledClaimBatch claims;claims.complete=true;claims.bookRevision=1;claims.claims={item};
        const std::vector<NativeResourceBalance> native{{703,103,2454,1,0,"bags"}};
        ProfessionPreparation restored;
        assert(PrepareCommissionTradeReadyRestore(ready,current,history,claims,native,2000,
            request.transition.receipt,restored,why));
        assert(restored.task.phase==Phase::Preparing && restored.task.context==current &&
            restored.task.checkpoint.data==ready.checkpoint.data && restored.task.checkpoint.step=="commission_trade_prepare");
        assert(restored.plan.statements.front().find("commission_craft_verified")!=std::string::npos);
        for(const auto& sql:restored.plan.statements)
            assert(sql.find("UPDATE living_activity_claim")==std::string::npos && sql.find("SET money")==std::string::npos);
        for(unsigned i=0;i<8;++i) {
            auto t=ready;auto h=history;auto c=claims;auto n=native;
            if(i==0)h.unresolvedOperation=true;
            if(i==1)h.commissionTrade.emplace_back();
            if(i==2)h.attempts.clear();
            if(i==3)c.claims.clear();
            if(i==4)n.front().quantity=0;
            if(i==5)t.phase=Phase::Executing;
            if(i==6)t.checkpoint.step="commission_trade_offer";
            if(i==7)n.front().actor=9;
            assert(!PrepareCommissionTradeReadyRestore(t,current,h,c,n,2000,request.transition.receipt,restored,why));
        }
    }
    {
        auto offered=request;offered.kind=offered.transition.task.checkpoint.step="commission_trade_offer";
        offered.effects=Mask(Effect::Inventory);offered.persistence=NativePersistence::JournalOnly;
        offered.consumption.clear();TestAdapter prepare(offered);prepare.offer=true;
        assert(ValidateOperationAdapter(offered,prepare,why));
        assert(ValidateOperationRequest(offered,task,task.context,nullptr,1000,why));
        // Compose the actual operation contract and authority gate: a lease
        // alone reproduces the old rejection; acknowledged intent dispatch
        // grants only the offer's inventory effect, never a fee transfer.
        ExecutionAuthority authority;authority.Observe(task.context,0);
        auto preparing=task;const auto lease=authority.Acquire(preparing,offered.effects,1000,60000);
        assert(lease.Granted());preparing.ownerGeneration=lease.lease.generation;
        auto a=offered.authorization;a.ownerGeneration=preparing.ownerGeneration;a.permittedEffects=offered.effects;
        assert(authority.Authorize({offered.effects,Lane::Managed,true},task.context,1001,&preparing,&a)==AuthorityCode::ReconciliationRequired);
        auto executing=offered.transition.task;
        const auto active=authority.Acquire(executing,offered.effects,1002,60000);assert(active.Granted());
        executing.ownerGeneration=active.lease.generation;a.revision=executing.revision;
        a.ownerGeneration=active.lease.generation;a.operation=offered.transition.receipt;
        assert(authority.BeginAtomic(active.lease,a.operation,1003).code==AuthorityCode::Allowed);
        assert(authority.BeginDispatch(active.lease,a.operation,1003).code==AuthorityCode::Allowed);
        assert(authority.Authorize({offered.effects,Lane::Managed,true},task.context,1003,&executing,&a)==AuthorityCode::Allowed);
        assert(authority.Authorize({Mask(Effect::Money),Lane::Managed,true},task.context,1003,&executing,&a)==AuthorityCode::EffectsDenied);
        auto progress=executing;
        assert(CanTransition(progress,Phase::Verifying));progress.phase=Phase::Verifying;
        assert(CanTransition(progress,Phase::WaitingExternal));progress.phase=Phase::WaitingExternal;
        assert(!CanTransition(progress,Phase::Preparing));
        assert(CanTransition(progress,Phase::Reconciling));progress.phase=Phase::Reconciling;
        assert(CanTransition(progress,Phase::Preparing));
        const auto plan=OperationRequestWrite(offered);
        assert(plan.receiptQuery.find(SqlValue("commission_trade_offer"))!=std::string::npos);
        for(const auto& sql:plan.statements) {
            assert(sql.find("UPDATE living_activity_claim")==std::string::npos);
            assert(sql.find("SET money")==std::string::npos);
        }
        prepare.offer=false;assert(!ValidateOperationAdapter(offered,prepare,why));
        for(unsigned i=0;i<10;++i) {
            auto bad=offered;
            if(i==0)bad.effects|=Mask(Effect::Money);
            if(i==1)bad.consumption=request.consumption;
            if(i==2)bad.itemGain={2454,1};
            if(i==3)bad.persistence=NativePersistence::Inventory;
            if(i==4)bad.transition.task.checkpoint.step="commission_trade";
            if(i==5)bad.beforeState="{}";
            if(i==6)bad.itemTransfer=item;
            if(i==7){auto changed=quote;++changed.items.front().quantity;bad.beforeState=EncodeCommissionTradeQuote(changed);}
            if(i==8)bad.transition.task.accepted=false;
            if(i==9){auto changed=quote;++changed.recipient;bad.beforeState=EncodeCommissionTradeQuote(changed);}
            TestAdapter rejected(bad);rejected.offer=true;
            assert(!ValidateOperationAdapter(bad,rejected,why));
            assert(!ValidateOperationRequest(bad,task,task.context,nullptr,1000,why));
        }
        auto mismatch=offered;mismatch.kind="vendor_purchase";TestAdapter wrong(mismatch);wrong.offer=true;
        assert(!ValidateOperationAdapter(mismatch,wrong,why));
    }
    assert(!ValidateOperationAdapter(request,adapter,why) && why=="native_commission_trade_adapter_required");
    adapter.trade=true;assert(ValidateOperationAdapter(request,adapter,why));
    assert(ValidateOperationRequest(request,task,task.context,nullptr,1000,why));
    assert(OperationRequestWrite(request).receiptQuery.find(SqlValue("commission_trade"))!=std::string::npos);
    assert(VerifyConsumedNativeResources(request,{{703,103,2454,1,0,"bags"}},{},why));
    assert(!VerifyConsumedNativeResources(request,{{703,103,2454,1,0,"bags"}},{{703,103,2454,1,0,"bags"}},why));
    {
        auto completed=task;completed.revision=7;completed.phase=Phase::Verifying;
        StoredCraftOperation stored;stored.acknowledged=true;stored.journalDigest=std::string(64,'a');
        auto& r=stored.receipt;r.id=request.transition.receipt;r.task=task.id;r.taskRevision=6;
        r.kind="commission_trade";r.state=OperationState::Verified;r.nativeReference="trade:"+r.id;
        r.evidence="native_commission_trade_and_fee_observed";
        stored.beforeState="{\"effects\":12,\"persistence\":1,\"native\":"+
            ClaimedNativeState(request.beforeState,request.consumption,8192)+'}';
        const CommissionTradeEvidence native{{{703,9,103,2454,1,{{65303,1,104,4,104,5}},true}},{703,9,220,380}};
        stored.afterState=ClaimedNativeState(EncodeCommissionTradeEvidence(quote,native),request.consumption,8192);
        CommissionTradeQuote decoded;assert(DecodeStoredCommissionTrade(completed,stored,decoded,why));
        ProfessionHistory history;history.task=task.id;history.revision=7;history.complete=true;
        history.unresolvedOperation=false;history.commissionTrade={stored};
        UnsettledClaimBatch claims;claims.complete=true;claims.bookRevision=1;
        ProfessionPreparation result;auto current=task.context;current.boot="c859a150-352b-4685-93c1-a35b7728e495";
        const std::string receipt="ff2efbdf-f0ec-4539-b840-299847970c02";
        assert(PrepareCommissionTradeSettlement(completed,current,history,claims,2000,receipt,result,why));
        assert(result.task.phase==Phase::Completed && result.task.checkpoint.step=="commission_completed" && result.task.context==current);
        assert(result.plan.statements.front().find("SHA2(CONCAT(o.before_state,'|',o.after_state),256)")!=std::string::npos);
        for(const auto& sql:result.plan.statements) {
            assert(sql.find("SET money")==std::string::npos && sql.find("INSERT INTO item_instance")==std::string::npos);
        }
        for(unsigned i=0;i<12;++i) {
            auto h=history;auto c=claims;auto t=completed;auto ctx=current;
            if(i==0)h.complete=false;
            if(i==1)h.unresolvedOperation=true;
            if(i==2)h.commissionTrade.push_back(stored);
            if(i==3)h.commissionTrade.front().acknowledged=false;
            if(i==4)h.commissionTrade.front().receipt.state=OperationState::Intent;
            if(i==5)h.commissionTrade.front().afterState="{}";
            if(i==6)h.commissionTrade.front().journalDigest.clear();
            if(i==7)c.claims.push_back(item);
            if(i==8)ctx.actor=9;
            if(i==9)t.phase=Phase::Completed;
            if(i==10)h.commissionMail.push_back(stored);
            if(i==11)h.commissionTrade.front().receipt.nativeReference="trade:other";
            assert(!PrepareCommissionTradeSettlement(t,ctx,h,c,2000,receipt,result,why));
        }
    }
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
