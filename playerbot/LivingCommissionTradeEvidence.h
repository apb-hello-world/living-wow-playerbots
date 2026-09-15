#pragma once
#include "LivingCommissionTradeContract.h"

namespace LivingActivity {
// Value-only journal evidence. Decoding never opens a native capture or changes
// its thread-local state, and does not turn untrusted text into a saved receipt.
struct CommissionTradeEvidence {
    std::vector<TradeItemTransfer> transfers;
    TradeCompletion completion;
};
inline std::string EncodeCommissionTradeEvidence(const CommissionTradeQuote& q,const CommissionTradeEvidence& e) {
    std::string why;
    if(!VerifyCommissionTradeRows(q,e.transfers,e.completion,why))throw std::invalid_argument(why);
    const auto& c=e.completion;const bool forward=c.first==q.actor;
    boost::property_tree::ptree p,rows;
    p.put("version",1);p.put("actor",q.actor);p.put("recipient",q.recipient);
    p.put("actor_money",forward?c.firstMoney:c.secondMoney);p.put("recipient_money",forward?c.secondMoney:c.firstMoney);
    for(const auto& row:e.transfers)for(const auto& d:row.destinations) {
        boost::property_tree::ptree item;
        item.put("source",row.item);item.put("position",d.position);item.put("quantity",d.quantity);
        item.put("before_item",d.beforeItem);item.put("before_count",d.beforeCount);
        item.put("after_item",d.afterItem);item.put("after_count",d.afterCount);rows.push_back({"",item});
    }
    p.add_child("transfers",rows);std::ostringstream out;boost::property_tree::write_json(out,p,false);
    if(out.str().size()>4096)throw std::invalid_argument("commission_trade_evidence_capacity");
    return out.str();
}
inline bool DecodeCommissionTradeEvidence(const CommissionTradeQuote& q,const std::string& text,
    CommissionTradeEvidence& result,std::string& why) {
    result={};why="commission_trade_evidence_invalid";
    if(text.empty() || text.size()>4096)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(text);boost::property_tree::read_json(in,p);
        CommissionTradeEvidence e;
        e.completion={p.get<uint32_t>("actor"),p.get<uint32_t>("recipient"),
            p.get<uint32_t>("actor_money"),p.get<uint32_t>("recipient_money")};
        const auto& transfers=p.get_child("transfers");
        if(!transfers.data().empty() || transfers.empty() || transfers.size()>256)return false;
        std::set<uint32_t> seen;
        for(const auto& row:transfers) {
            if(!row.first.empty())return false;
            const auto& d=row.second;const auto source=d.get<uint32_t>("source");
            if(e.transfers.empty() || e.transfers.back().item!=source) {
                if(!source || !seen.insert(source).second || e.transfers.size()>=6)return false;
                e.transfers.push_back({q.actor,q.recipient,source,q.entry,0,{},true});
            }
            TradeDestination part{d.get<uint16_t>("position"),d.get<uint32_t>("quantity"),d.get<uint32_t>("before_item"),
                d.get<uint32_t>("before_count"),d.get<uint32_t>("after_item"),d.get<uint32_t>("after_count")};
            auto& transfer=e.transfers.back();
            if(uint64_t(transfer.quantity)+part.quantity>UINT32_MAX)return false;
            transfer.quantity+=part.quantity;transfer.destinations.push_back(part);
        }
        if(!VerifyCommissionTradeRows(q,e.transfers,e.completion,why) || EncodeCommissionTradeEvidence(q,e)!=text)return false;
        result=std::move(e);why.clear();return true;
    }catch(const std::exception&){return false;}
}
}
