#pragma once
#include "LivingCommissionTrade.h"
#include "LivingCommissionJob.h"
#include "LivingActivityClaimConsumption.h"
#include <algorithm>

namespace LivingActivity {
// Immutable operation input. No client/model-supplied trade window data is
// authority; the native adapter must independently rebuild this same quote.
inline std::string EncodeCommissionTradeQuote(const CommissionTradeQuote& q) {
    if(!ValidCommissionTradeQuote(q))throw std::invalid_argument("exact_commission_trade_quote_required");
    boost::property_tree::ptree p,items;p.put("version",1);
    p.put("actor",q.actor);p.put("recipient",q.recipient);p.put("entry",q.entry);p.put("fee",q.fee);
    p.put("actor_money",q.actorMoney);p.put("recipient_money",q.recipientMoney);
    for(const auto& item:q.items) {
        boost::property_tree::ptree row;row.put("item",item.item);row.put("quantity",item.quantity);
        items.push_back({"",row});
    }
    p.add_child("items",items);std::ostringstream out;boost::property_tree::write_json(out,p,false);return out.str();
}
inline bool DecodeCommissionTradeQuote(const std::string& text,CommissionTradeQuote& result) {
    result={};if(text.empty() || text.size()>4096)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(text);boost::property_tree::read_json(in,p);
        CommissionTradeQuote q;q.actor=p.get<uint32_t>("actor");q.recipient=p.get<uint32_t>("recipient");
        q.entry=p.get<uint32_t>("entry");q.fee=p.get<uint32_t>("fee");
        q.actorMoney=p.get<uint32_t>("actor_money");q.recipientMoney=p.get<uint32_t>("recipient_money");
        const auto& items=p.get_child("items");
        if(!items.data().empty() || items.empty() || items.size()>6)return false;
        for(const auto& item:items) {
            if(!item.first.empty())return false;
            q.items.push_back({item.second.get<uint32_t>("item"),item.second.get<uint32_t>("quantity")});
        }
        // Canonical bytes reject duplicate/unknown fields, alternate versions,
        // overflow, scalar/object confusion and silently truncated quantities.
        if(!ValidCommissionTradeQuote(q) || EncodeCommissionTradeQuote(q)!=text)return false;
        result=std::move(q);return true;
    }catch(const std::exception&){return false;}
}
inline bool MatchesCommissionTradeOutput(const Task& task,const CommissionTradeQuote& q) {
    CommissionJob job;ProfessionJob recipe;std::string why;
    if(!ValidCommissionTradeQuote(q) || !Validate(task,why) || !IsCommissionJob(task) ||
        !ValidateCommissionTask(task,why) || !DecodeCommissionJob(task.checkpoint.data,job,why) ||
        !task.accepted || task.mode!=Mode::Active || Terminal(task.phase) || !job.craftFinishedRevision ||
        (job.agreement.delivery!="direct" && job.agreement.delivery!="meeting") ||
        job.agreement.actor!=q.actor || job.agreement.recipient!=q.recipient || job.agreement.feeCopper!=q.fee ||
        !DecodeProfessionIntent(job.craft,recipe,why) || recipe.outputEntry!=q.entry)return false;
    uint64_t total=0;for(const auto& item:q.items)total+=item.quantity;
    return total==recipe.outputQuantity;
}
inline bool ExactCommissionTradeConsumption(const Task& task,const CommissionTradeQuote& q,
    const std::vector<ClaimConsumption>& uses) {
    if(!MatchesCommissionTradeOutput(task,q) || uses.empty() || uses.size()>16)return false;
    std::map<uint32_t,uint64_t> remaining;
    for(const auto& item:q.items)remaining[item.item]=item.quantity;
    std::set<std::string> ids;
    for(const auto& use:uses) {
        const auto& c=use.before;
        if(!ValidResourceClaim(c) || c.task!=task.id || c.actor!=q.actor || c.location!="bags" ||
            c.state!="held" || c.nativeReference || c.itemEntry!=q.entry || c.copper || !c.quantity ||
            c.quantity!=use.used || !ids.insert(c.id).second || !remaining.count(c.itemGuid) ||
            remaining.at(c.itemGuid)<c.quantity)return false;
        remaining.at(c.itemGuid)-=c.quantity;
    }
    return std::all_of(remaining.begin(),remaining.end(),[](const auto& row){return row.second==0;});
}
}
