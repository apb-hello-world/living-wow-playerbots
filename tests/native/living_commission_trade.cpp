#include "LivingCommissionTrade.h"
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
        NativeTradeCapture capture;
        assert(!VerifyCommissionTrade(quote,capture,why));
        NativeTradeCapture::Observe(row());
        assert(!VerifyCommissionTrade(quote,capture,why)); // window disappearance is not proof
        NativeTradeCapture::Complete({20,28,481,1120});
        assert(VerifyCommissionTrade(quote,capture,why));
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
