#pragma once
#include "LivingTradeCapture.h"
#include <map>
#include <string>

namespace LivingActivity {
struct CommissionTradeItem {uint32_t item=0,quantity=0;};
struct CommissionTradeQuote {
    uint32_t actor=0,recipient=0,entry=0,fee=0,actorMoney=0,recipientMoney=0;
    std::vector<CommissionTradeItem> items;
};
inline bool ValidCommissionTradeQuote(const CommissionTradeQuote& q) {
    if(!q.actor || !q.recipient || q.actor==q.recipient || !q.entry || q.items.empty() || q.items.size()>6 ||
        q.recipientMoney<q.fee || uint64_t(q.actorMoney)+q.fee>UINT32_MAX)return false;
    std::set<uint32_t> items;
    for(const auto& item:q.items)if(!item.item || !item.quantity || !items.insert(item.item).second)return false;
    return true;
}
// This proves the observed exchange against an already validated quote. It
// does not validate consent, acquire a lease, persist intent or finish a task.
inline bool VerifyCommissionTrade(const CommissionTradeQuote& q,const NativeTradeCapture& capture,std::string& why) {
    auto reject=[&](const char* reason){why=reason;return false;};
    if(!ValidCommissionTradeQuote(q))return reject("commission_trade_quote_invalid");
    if(!capture.Completed())return reject("commission_trade_native_completion_missing");
    const auto& completion=capture.Completion();
    const bool forward=completion.first==q.actor && completion.second==q.recipient;
    if(!forward && !(completion.first==q.recipient && completion.second==q.actor))
        return reject("commission_trade_participants_changed");
    const auto actorMoney=forward?completion.firstMoney:completion.secondMoney;
    const auto recipientMoney=forward?completion.secondMoney:completion.firstMoney;
    if(uint64_t(q.actorMoney)+q.fee!=actorMoney || q.recipientMoney-q.fee!=recipientMoney)
        return reject("commission_trade_fee_not_observed");
    if(capture.Rows().size()!=q.items.size())return reject("commission_trade_output_count_changed");
    std::set<uint32_t> sources;
    std::map<uint16_t,std::pair<uint32_t,uint32_t>> latest;
    std::map<uint32_t,uint16_t> locations;
    for(const auto& transfer:capture.Rows()) {
        if(!VerifiedTradeTransfer(transfer) || transfer.sender!=q.actor || transfer.receiver!=q.recipient || transfer.entry!=q.entry ||
            !sources.insert(transfer.item).second)return reject("commission_trade_output_changed");
        bool matched=false;
        for(const auto& expected:q.items)if(expected.item==transfer.item && expected.quantity==transfer.quantity)matched=true;
        if(!matched)return reject("commission_trade_output_changed");
        for(const auto& destination:transfer.destinations) {
            const auto earlier=latest.find(destination.position);
            if(earlier!=latest.end() && earlier->second!=std::make_pair(destination.beforeItem,destination.beforeCount))
                return reject("commission_trade_merge_chain_changed");
            const auto location=locations.find(destination.afterItem);
            if(location!=locations.end() && location->second!=destination.position)
                return reject("commission_trade_item_location_changed");
            latest[destination.position]={destination.afterItem,destination.afterCount};
            locations[destination.afterItem]=destination.position;
        }
    }
    why.clear();return true;
}
}
