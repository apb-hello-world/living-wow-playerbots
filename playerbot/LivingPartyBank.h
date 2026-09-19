#pragma once
#include "LivingActivityTransfer.h"
#include <boost/property_tree/json_parser.hpp>
#include <sstream>
#include <optional>

namespace LivingActivity {
// Snapshot at acceptance, not a live query for everything now considered junk.
// Newly acquired items never enter an already accepted cleanup batch.
struct PartyBankItem { uint32_t guid=0,entry=0,quantity=0; };
struct PartyBankJob {
    std::vector<PartyBankItem> items;
    uint32_t next=0;
};
constexpr size_t PartyBankBatchLimit=32;
inline bool PartyBankFailureBackoff(bool nativeAttempted,OperationState outcome) {
    return nativeAttempted && outcome==OperationState::Rejected;
}
inline std::optional<Phase> PartyBankResumePhase(Phase phase) {
    if(phase==Phase::Paused || phase==Phase::Deferred || phase==Phase::WaitingExternal)return Phase::Reconciling;
    if(phase==Phase::Queued || phase==Phase::Reconciling)return Phase::Preparing;
    return {};
}
inline bool ValidPartyBankJob(const PartyBankJob& job) {
    if(job.items.empty() || job.items.size()>PartyBankBatchLimit || job.next>job.items.size())return false;
    uint32_t previous=0;
    for(const auto& item:job.items) {
        if(!item.guid || item.guid<=previous || !item.entry || !item.quantity)return false;
        previous=item.guid;
    }
    return true;
}
inline std::string EncodePartyBankJob(const PartyBankJob& job) {
    std::string value="{\"workflow\":\"party_bank_v1\",\"next\":"+std::to_string(job.next)+",\"items\":[";
    bool first=true;
    for(const auto& item:job.items) {
        if(!first)value+=',';
        first=false;
        value+="{\"guid\":"+std::to_string(item.guid)+",\"entry\":"+std::to_string(item.entry)+
            ",\"quantity\":"+std::to_string(item.quantity)+'}';
    }
    return value+"]}";
}
inline bool DecodePartyBankJob(const std::string& value,PartyBankJob& job) {
    job={};if(value.size()>4096)return false;
    try {
        boost::property_tree::ptree p;std::istringstream input(value);boost::property_tree::read_json(input,p);
        if(p.get<std::string>("workflow")!="party_bank_v1")return false;
        PartyBankJob parsed;parsed.next=p.get<uint32_t>("next");
        for(const auto& row:p.get_child("items")) {
            if(!row.first.empty() || parsed.items.size()>=PartyBankBatchLimit)return false;
            parsed.items.push_back({row.second.get<uint32_t>("guid"),row.second.get<uint32_t>("entry"),
                row.second.get<uint32_t>("quantity")});
        }
        // Canonical form rejects duplicate/unknown keys, coercions and trailing payloads.
        if(!ValidPartyBankJob(parsed) || EncodePartyBankJob(parsed)!=value)return false;
        job=std::move(parsed);return true;
    } catch(...) {return false;}
}
inline bool IsPartyBankTask(const Task& task) {
    return task.source=="party_bank" && task.kind==Kind::PartyErrand &&
        task.id==task.root && task.parent.empty();
}
inline bool ValidatePartyBankTask(const Task& task,std::string& why) {
    if(task.source!="party_bank")return true;
    PartyBankJob job;
    if(!IsPartyBankTask(task) || !DecodePartyBankJob(task.checkpoint.data,job) ||
        (task.checkpoint.step!="party_bank_prepare" && task.checkpoint.step!="bank_deposit" &&
         task.checkpoint.step!="profession_service_bank") ||
        (task.phase==Phase::Queued && job.next) ||
        ((task.phase==Phase::Completed)!=(job.next==job.items.size()))) {
        why="invalid_party_bank_task";return false;
    }
    return true;
}
inline bool PartyBankItemMatches(const PartyBankJob& job,const PartyBankItem& item) {
    if(!ValidPartyBankJob(job) || job.next>=job.items.size())return false;
    const auto& expected=job.items[job.next];
    return expected.guid==item.guid && expected.entry==item.entry && expected.quantity==item.quantity;
}
inline bool PreservePartyBankIntent(const Task& before,const Task& after,std::string& why) {
    if(before.source!="party_bank")return true;
    // Ordinary admission cannot add/remove stacks or advance the deposit cursor.
    // Only the acknowledged native-deposit outcome may update progress atomically.
    if(!IsPartyBankTask(after) || before.checkpoint.data!=after.checkpoint.data) {
        why="party_bank_intent_requires_native_receipt";return false;
    }
    return true;
}
inline bool AcknowledgePartyBankDeposit(Task& after,const PartyBankItem& item,
    const ResourceClaim& consumed,const OperationResult& proof) {
    PartyBankJob job;std::string why;
    if(!IsPartyBankTask(after) || !ValidatePartyBankTask(after,why) ||
        after.phase!=Phase::Verifying || !DecodePartyBankJob(after.checkpoint.data,job) ||
        !PartyBankItemMatches(job,item) ||
        !ValidBankDeposit(consumed) || consumed.task!=after.id || consumed.actor!=after.actor ||
        consumed.itemGuid!=item.guid || consumed.itemEntry!=item.entry || consumed.quantity!=item.quantity || !IsUuid(proof.id) ||
        proof.task!=after.id || !after.revision || proof.taskRevision!=after.revision-1 ||
        proof.kind!="bank_deposit" || proof.state!=OperationState::Verified ||
        proof.nativeReference!="bank_item:"+std::to_string(item.guid) ||
        proof.evidence!="native_bank_stack_deposited")return false;
    ++job.next;after.checkpoint.data=EncodePartyBankJob(job);
    if(job.next==job.items.size())after.phase=Phase::Completed;
    return true;
}
}
