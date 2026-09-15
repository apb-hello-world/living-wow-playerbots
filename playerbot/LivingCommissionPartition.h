#pragma once
#include "LivingCommissionTradeContract.h"
#include "LivingActivityClaimCodec.h"

namespace LivingActivity {
// One native bag-stack partition. The claimed GUID and every claim remain
// unchanged; only uncommitted surplus moves to a new, initially empty slot.
struct CommissionPartitionQuote {
    uint32_t actor=0,recipient=0,item=0,entry=0,count=0,quantity=0,money=0;
    uint16_t position=0,destination=0;
    uint32_t sourceBag=0,destinationBag=0;
    std::vector<ResourceClaim> claims;
};
inline bool ValidCommissionPartition(const CommissionPartitionQuote& q) {
    if(!q.actor || !q.recipient || q.actor==q.recipient || !q.item || !q.entry || !q.quantity ||
        q.count<=q.quantity || !q.position || !q.destination || q.position==q.destination ||
        ((q.position>>8)==255 ? bool(q.sourceBag) : !q.sourceBag) ||
        ((q.destination>>8)==255 ? bool(q.destinationBag) : !q.destinationBag) ||
        q.claims.empty() || q.claims.size()>16)return false;
    uint64_t quantity=0;std::string previous,task;
    for(const auto& c:q.claims) {
        if(!ValidResourceClaim(c) || c.actor!=q.actor || c.itemGuid!=q.item || c.itemEntry!=q.entry ||
            c.copper || c.nativeReference || c.state!="held" || c.location!="bags" ||
            (!previous.empty() && c.id<=previous) || (!task.empty() && c.task!=task))return false;
        previous=c.id;task=c.task;quantity+=c.quantity;
    }
    return quantity==q.quantity;
}
inline std::string EncodeCommissionPartition(const CommissionPartitionQuote& q) {
    if(!ValidCommissionPartition(q))throw std::invalid_argument("exact_commission_partition_required");
    boost::property_tree::ptree p,claims;
    p.put("version",1);p.put("actor",q.actor);p.put("recipient",q.recipient);p.put("item",q.item);p.put("entry",q.entry);
    p.put("count",q.count);p.put("quantity",q.quantity);p.put("money",q.money);p.put("position",q.position);
    p.put("destination",q.destination);p.put("source_bag",q.sourceBag);p.put("destination_bag",q.destinationBag);
    for(const auto& c:q.claims) {
        boost::property_tree::ptree row;
        row.put("id",c.id);row.put("task",c.task);row.put("actor",c.actor);row.put("item_guid",c.itemGuid);
        row.put("item_entry",c.itemEntry);row.put("quantity",c.quantity);row.put("copper",c.copper);
        row.put("location",c.location);row.put("reference",c.nativeReference);row.put("state",c.state);row.put("revision",c.revision);
        claims.push_back({"",row});
    }
    p.add_child("claims",claims);std::ostringstream out;boost::property_tree::write_json(out,p,false);
    if(out.str().size()>4096)throw std::invalid_argument("commission_partition_quote_bound");
    return out.str();
}
inline bool DecodeCommissionPartition(const std::string& text,CommissionPartitionQuote& result) {
    result={};if(text.empty() || text.size()>4096)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(text);boost::property_tree::read_json(in,p);
        CommissionPartitionQuote q;
        q.actor=p.get<uint32_t>("actor");q.recipient=p.get<uint32_t>("recipient");q.item=p.get<uint32_t>("item");
        q.entry=p.get<uint32_t>("entry");q.count=p.get<uint32_t>("count");q.quantity=p.get<uint32_t>("quantity");
        q.money=p.get<uint32_t>("money");q.position=p.get<uint16_t>("position");q.destination=p.get<uint16_t>("destination");
        q.sourceBag=p.get<uint32_t>("source_bag");q.destinationBag=p.get<uint32_t>("destination_bag");
        const auto& rows=p.get_child("claims");if(!rows.data().empty() || rows.empty() || rows.size()>16)return false;
        for(const auto& row:rows) {
            if(!row.first.empty())return false;
            std::ostringstream encoded;boost::property_tree::write_json(encoded,row.second,false);
            ResourceClaim c;std::string why;if(!DecodeClaimProjection(encoded.str(),c,why))return false;q.claims.push_back(c);
        }
        if(!ValidCommissionPartition(q) || EncodeCommissionPartition(q)!=text)return false;
        result=std::move(q);return true;
    }catch(const std::exception&){return false;}
}
inline bool MatchesCommissionPartition(const Task& task,const CommissionPartitionQuote& q) {
    CommissionJob job;ProfessionJob recipe;std::string why;
    return ValidCommissionPartition(q) && Validate(task,why) && ValidateCommissionTask(task,why) &&
        DecodeCommissionJob(task.checkpoint.data,job,why) && DecodeProfessionIntent(job.craft,recipe,why) &&
        task.accepted && task.mode==Mode::Active && !Terminal(task.phase) && job.craftFinishedRevision &&
        (job.agreement.delivery=="direct" || job.agreement.delivery=="meeting") &&
        task.actor==q.actor && task.root==task.id && job.agreement.recipient==q.recipient && recipe.outputEntry==q.entry &&
        q.quantity<=recipe.outputQuantity && q.claims.front().task==task.id;
}
inline bool VerifyCommissionPartitionBalances(const CommissionPartitionQuote& q,
    const std::vector<NativeResourceBalance>& before,const std::vector<NativeResourceBalance>& after,
    const NativeResourceBalance& surplus) {
    if(!ValidCommissionPartition(q) || before.size()!=1 || after.size()!=2 || !surplus.itemGuid || surplus.itemGuid==q.item ||
        surplus.actor!=q.actor || surplus.itemEntry!=q.entry || surplus.quantity!=q.count-q.quantity ||
        surplus.copper || surplus.nativeReference || surplus.location!="bags")return false;
    auto matches=[&](const NativeResourceBalance& b,uint32_t item,uint32_t count) {
        return b.actor==q.actor && b.itemGuid==item && b.itemEntry==q.entry && b.quantity==count &&
            !b.copper && !b.nativeReference && b.location=="bags";
    };
    return matches(before.front(),q.item,q.count) &&
        ((matches(after[0],q.item,q.quantity) && matches(after[1],surplus.itemGuid,surplus.quantity)) ||
         (matches(after[1],q.item,q.quantity) && matches(after[0],surplus.itemGuid,surplus.quantity)));
}
}
