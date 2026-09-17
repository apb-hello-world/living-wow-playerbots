#pragma once
#include "LivingCapacityPreparation.h"
#include <boost/property_tree/json_parser.hpp>
#include <sstream>
#include <optional>

namespace LivingActivity {
// Snapshot at acceptance, not a live query for everything now considered junk.
// Newly acquired items never enter an already accepted cleanup batch.
struct PartyVendorItem { uint32_t guid=0,entry=0,quantity=0; };
struct PartyVendorJob {
    std::vector<PartyVendorItem> items;
    uint32_t next=0;
};
constexpr size_t PartyVendorBatchLimit=32;
inline std::optional<Phase> PartyVendorResumePhase(Phase phase) {
    if(phase==Phase::Paused || phase==Phase::Deferred || phase==Phase::WaitingExternal)return Phase::Reconciling;
    if(phase==Phase::Queued || phase==Phase::Reconciling)return Phase::Preparing;
    return {};
}
inline bool ValidPartyVendorJob(const PartyVendorJob& job) {
    if(job.items.empty() || job.items.size()>PartyVendorBatchLimit || job.next>job.items.size())return false;
    uint32_t previous=0;
    for(const auto& item:job.items) {
        if(!item.guid || item.guid<=previous || !item.entry || !item.quantity)return false;
        previous=item.guid;
    }
    return true;
}
inline std::string EncodePartyVendorJob(const PartyVendorJob& job) {
    std::string value="{\"workflow\":\"party_vendor_v1\",\"next\":"+std::to_string(job.next)+",\"items\":[";
    bool first=true;
    for(const auto& item:job.items) {
        if(!first)value+=',';
        first=false;
        value+="{\"guid\":"+std::to_string(item.guid)+",\"entry\":"+std::to_string(item.entry)+
            ",\"quantity\":"+std::to_string(item.quantity)+'}';
    }
    return value+"]}";
}
inline bool DecodePartyVendorJob(const std::string& value,PartyVendorJob& job) {
    job={};if(value.size()>4096)return false;
    try {
        boost::property_tree::ptree p;std::istringstream input(value);boost::property_tree::read_json(input,p);
        if(p.get<std::string>("workflow")!="party_vendor_v1")return false;
        PartyVendorJob parsed;parsed.next=p.get<uint32_t>("next");
        for(const auto& row:p.get_child("items")) {
            if(!row.first.empty() || parsed.items.size()>=PartyVendorBatchLimit)return false;
            parsed.items.push_back({row.second.get<uint32_t>("guid"),row.second.get<uint32_t>("entry"),
                row.second.get<uint32_t>("quantity")});
        }
        // Canonical form rejects duplicate/unknown keys, coercions and trailing payloads.
        if(!ValidPartyVendorJob(parsed) || EncodePartyVendorJob(parsed)!=value)return false;
        job=std::move(parsed);return true;
    } catch(...) {return false;}
}
inline bool IsPartyVendorTask(const Task& task) {
    return task.source=="party_vendor" && task.kind==Kind::PartyErrand &&
        task.id==task.root && task.parent.empty();
}
inline bool ValidatePartyVendorTask(const Task& task,std::string& why) {
    if(task.source!="party_vendor")return true;
    PartyVendorJob job;
    if(!IsPartyVendorTask(task) || !DecodePartyVendorJob(task.checkpoint.data,job) ||
        (task.checkpoint.step!="party_vendor_prepare" && task.checkpoint.step!="party_vendor_sale" &&
         task.checkpoint.step!="profession_service_capacity_vendor") ||
        (task.phase==Phase::Queued && job.next) ||
        ((task.phase==Phase::Completed)!=(job.next==job.items.size()))) {
        why="invalid_party_vendor_task";return false;
    }
    return true;
}
inline bool PartyVendorItemMatches(const PartyVendorJob& job,const CapacitySaleFacts& item) {
    if(!ValidPartyVendorJob(job) || job.next>=job.items.size())return false;
    const auto& expected=job.items[job.next];
    return expected.guid==item.guid && expected.entry==item.entry && expected.quantity==item.quantity;
}
inline bool PreservePartyVendorIntent(const Task& before,const Task& after,std::string& why) {
    if(before.source!="party_vendor")return true;
    // Ordinary admission cannot add/remove stacks or advance the sale cursor.
    // Only the acknowledged native-sale outcome may update progress atomically.
    if(!IsPartyVendorTask(after) || before.checkpoint.data!=after.checkpoint.data) {
        why="party_vendor_intent_requires_native_receipt";return false;
    }
    return true;
}
inline bool AcknowledgePartyVendorSale(Task& after,const CapacitySaleFacts& item,
    const ResourceClaim& consumed,const OperationResult& proof) {
    PartyVendorJob job;std::string why;uint32_t price=0;
    if(!IsPartyVendorTask(after) || !ValidatePartyVendorTask(after,why) ||
        after.phase!=Phase::Verifying || !DecodePartyVendorJob(after.checkpoint.data,job) ||
        !PartyVendorItemMatches(job,item) || item.actor!=after.actor || !QuoteCapacitySale(item,price) ||
        !ExactCapacityClaim(consumed,item,after.id) || !IsUuid(proof.id) ||
        proof.task!=after.id || !after.revision || proof.taskRevision!=after.revision-1 ||
        proof.kind!="party_vendor_sale" || proof.state!=OperationState::Verified ||
        proof.nativeReference!="vendor_sale:"+std::to_string(item.guid) ||
        proof.evidence!="native_party_sale_money_item_and_slot_observed")return false;
    ++job.next;after.checkpoint.data=EncodePartyVendorJob(job);
    if(job.next==job.items.size())after.phase=Phase::Completed;
    return true;
}
}
