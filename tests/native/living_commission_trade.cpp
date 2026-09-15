#include "LivingCommissionTradeContract.h"
#include "LivingCommissionTradeEvidence.h"
#include <cassert>
#include <iostream>
using namespace LivingActivity;

TradeItemTransfer row(uint32_t item=100,uint32_t quantity=5) {
    return {20,28,item,2454,quantity,{{23,quantity,200,10,200,10+quantity}},true};
}
int main() {
    const CommissionTradeQuote quote{20,28,2454,60,421,1180,{{100,5}}};
    std::string why;
    {
        CommissionTradeQuote decoded;const auto text=EncodeCommissionTradeQuote(quote);
        assert(DecodeCommissionTradeQuote(text,decoded) && EncodeCommissionTradeQuote(decoded)==text);
        auto duplicate=text;duplicate.insert(1,"\"fee\":\"0\",");assert(!DecodeCommissionTradeQuote(duplicate,decoded));
        auto unknown=text;unknown.insert(1,"\"complete\":true,");assert(!DecodeCommissionTradeQuote(unknown,decoded));
        auto overflow=text;const auto offset=overflow.find("\"60\"");assert(offset!=std::string::npos);
        overflow.replace(offset,4,"\"4294967296\"");assert(!DecodeCommissionTradeQuote(overflow,decoded));
        ProfessionJob recipe;recipe.recipe=2329;recipe.skill=171;recipe.initialSkill=75;
        recipe.purpose=ProfessionPurpose::RequestedItem;recipe.outputEntry=2454;recipe.outputQuantity=5;
        recipe.reagents={{765,1},{2449,1},{3371,1}};
        CommissionJob job;job.agreement={"lwc-123","native-trade-order","direct",EncodeProfessionJob(recipe),20,28,60,1000};
        job.craft=job.agreement.recipe;job.craftFinishedRevision=5;
        Task task;task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f92";task.actor=task.context.actor=20;
        task.kind=Kind::Commission;task.source="commission_job";task.sourceKey=job.agreement.id;
        task.revision=5;task.mode=Mode::Active;task.accepted=true;task.phase=Phase::Preparing;
        task.createdAtMs=task.updatedAtMs=1000;task.checkpoint.data=EncodeCommissionJob(job);
        task.context.boot="ff2efbdf-f0ec-4539-b840-299847970c00";
        task.context.actorGeneration=task.context.mapGeneration=task.context.policyRevision=1;
        ResourceClaim held;held.id="d1879146-6e96-4e71-827d-6d12d965edb7";held.task=task.id;
        held.actor=20;held.itemGuid=100;held.itemEntry=2454;held.quantity=5;held.location="bags";held.state="held";
        const std::vector<ClaimConsumption> uses{{held,5}};
        assert(ExactCommissionTradeConsumption(task,quote,uses));
        auto split=uses;split.front().before.quantity=2;split.front().used=2;
        auto extra=held;extra.id="d1879146-6e96-4e71-827d-6d12d965edb8";extra.quantity=3;split.push_back({extra,3});
        assert(ExactCommissionTradeConsumption(task,quote,split)); // same owned stack, distinct paid craft claims
        for(int bad=0;bad<9;++bad) {
            auto changed=uses;auto t=task;auto q=quote;
            if(bad==0)changed.front().before.actor=28;
            if(bad==1)changed.front().before.location="bank";
            if(bad==2)changed.front().used=4;
            if(bad==3)changed.push_back(changed.front());
            if(bad==4)q.fee=59;
            if(bad==5)t.accepted=false;
            if(bad==6){auto j=job;j.agreement.delivery="mail";t.checkpoint.data=EncodeCommissionJob(j);}
            if(bad==7)t.phase=Phase::Failed;
            if(bad==8){t.phase=Phase::Completed;t.checkpoint.step="commission_completed";}
            assert(!ExactCommissionTradeConsumption(t,q,changed));
        }
    }
    {
        NativeTradeCapture capture;
        assert(!VerifyCommissionTrade(quote,capture,why));
        NativeTradeCapture::Observe(row());
        assert(!VerifyCommissionTrade(quote,capture,why)); // window disappearance is not proof
        NativeTradeCapture::Complete({20,28,481,1120});
        assert(VerifyCommissionTrade(quote,capture,why));
        const auto encoded=EncodeCommissionTradeEvidence(quote,{capture.Rows(),capture.Completion()});
        CommissionTradeEvidence stored;assert(DecodeCommissionTradeEvidence(quote,encoded,stored,why));
        assert(NativeTradeCapture::Active() && capture.Valid()); // decoding cannot corrupt live native scopes
        assert(EncodeCommissionTradeEvidence(quote,stored)==encoded);
        auto duplicate=encoded;duplicate.insert(1,"\"actor_money\":\"0\",");
        assert(!DecodeCommissionTradeEvidence(quote,duplicate,stored,why));
        auto overflow=encoded;const auto at=overflow.find("\"23\"");assert(at!=std::string::npos);
        overflow.replace(at,4,"\"65536\"");assert(!DecodeCommissionTradeEvidence(quote,overflow,stored,why));
        assert(!DecodeCommissionTradeEvidence(quote,"{}",stored,why));
        auto changed=quote;changed.fee=59;assert(!VerifyCommissionTrade(changed,capture,why));
        changed=quote;changed.recipient=29;assert(!VerifyCommissionTrade(changed,capture,why));
        changed=quote;changed.items[0].quantity=4;assert(!VerifyCommissionTrade(changed,capture,why));
        changed=quote;changed.items[0].item=101;assert(!VerifyCommissionTrade(changed,capture,why));
    }
    assert(!NativeTradeCapture::Active());
    NativeTradeCapture::Observe(row()); // no scope, no hidden persisted observation
    {
        NativeTradeCapture capture;
        auto fresh=row();fresh.destinations={{24,5,0,0,100,5}};
        NativeTradeCapture::Observe(fresh);
        NativeTradeCapture::Complete({28,20,1120,481});
        assert(VerifyCommissionTrade(quote,capture,why));
    }
    {
        auto combined=quote;combined.items.push_back({101,5});
        NativeTradeCapture capture;
        NativeTradeCapture::Observe(row());
        auto second=row(101);second.destinations={{23,5,200,15,200,20}};
        NativeTradeCapture::Observe(second);
        NativeTradeCapture::Complete({20,28,481,1120});
        assert(VerifyCommissionTrade(combined,capture,why));
        assert(!VerifyCommissionTrade(quote,capture,why)); // extra goods never silently accepted
    }
    {
        auto combined=quote;combined.items.push_back({101,5});
        NativeTradeCapture capture;NativeTradeCapture::Observe(row());
        NativeTradeCapture::Observe(row(101)); // contradictory second before-count
        NativeTradeCapture::Complete({20,28,481,1120});
        assert(!VerifyCommissionTrade(combined,capture,why));
        assert(why=="commission_trade_merge_chain_changed");
    }
    for(int bad=0;bad<6;++bad) {
        NativeTradeCapture capture;auto transfer=row();
        if(bad==0)transfer.destinations[0].afterCount=14;
        if(bad==1)transfer.destinations[0].afterItem=201;
        if(bad==2)transfer.destinations.push_back(transfer.destinations.front());
        if(bad==3)transfer.quantity=6;
        if(bad==4)transfer.sender=0;
        if(bad==5)transfer.valid=false;
        NativeTradeCapture::Observe(transfer);NativeTradeCapture::Complete({20,28,481,1120});
        assert(!capture.Valid() && !VerifyCommissionTrade(quote,capture,why));
    }
    {
        NativeTradeCapture outer;
        {NativeTradeCapture inner;assert(!inner.Valid());}
        NativeTradeCapture::Observe(row());NativeTradeCapture::Complete({20,28,481,1120});
        assert(!outer.Valid());
    }
    {
        NativeTradeCapture capture;
        NativeTradeCapture::Observe(row());NativeTradeCapture::Complete({20,28,481,1120});
        NativeTradeCapture::Complete({20,28,481,1120});assert(!capture.Valid());
    }
    {
        NativeTradeCapture capture;auto split=row();
        split.destinations={{23,2,200,18,200,20},{24,3,0,0,100,3}};
        NativeTradeCapture::Observe(split);NativeTradeCapture::Complete({20,28,481,1120});
        assert(VerifyCommissionTrade(quote,capture,why));
        NativeTradeCapture::Observe(row(101));assert(!capture.Valid());
    }
    for(int bad=0;bad<3;++bad) {
        NativeTradeCapture capture;auto changed=row();
        if(bad==0)std::swap(changed.sender,changed.receiver);
        if(bad==1)changed.entry=2770;
        if(bad==2)changed.destinations[0].beforeItem=changed.item;
        NativeTradeCapture::Observe(changed);NativeTradeCapture::Complete({20,28,481,1120});
        assert(!VerifyCommissionTrade(quote,capture,why));
    }
    {
        NativeTradeCapture capture;
        for(uint32_t n=0;n<13;++n)NativeTradeCapture::Observe(row(100+n));
        assert(!capture.Valid() && capture.Rows().size()==12);
    }
    auto overflow=quote;overflow.actorMoney=UINT32_MAX;assert(!ValidCommissionTradeQuote(overflow));
    auto unpaid=quote;unpaid.recipientMoney=59;assert(!ValidCommissionTradeQuote(unpaid));
    std::cout<<"native trade capture and exact commission exchange predicates passed\n";
}
