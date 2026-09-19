#pragma once
#include "LivingAuctionPost.h"
#include <optional>
namespace LivingActivity {
struct PartyAuctionJob {std::vector<AuctionPostItem> items;uint32_t next=0;};
constexpr size_t PartyAuctionBatchLimit=20;
inline std::optional<Phase> PartyAuctionResumePhase(Phase phase) {
    if(phase==Phase::Paused || phase==Phase::Deferred || phase==Phase::WaitingExternal)return Phase::Reconciling;
    if(phase==Phase::Queued || phase==Phase::Reconciling)return Phase::Preparing;
    return {};
}
inline bool IsPartyAuctionTask(const Task& t) {
    return t.source=="party_auction" && t.kind==Kind::PartyErrand && t.id==t.root && t.parent.empty();
}
inline bool ValidPartyAuctionJob(const PartyAuctionJob& j) {
    if(j.items.empty() || j.items.size()>PartyAuctionBatchLimit || j.next>j.items.size())return false;
    uint32_t previous=0;
    for(const auto& i:j.items) {if(!ValidAuctionPostItem(i) || i.guid<=previous)return false;previous=i.guid;}
    return true;
}
inline std::string EncodePartyAuctionJob(const PartyAuctionJob& j) {
    std::string out="{\"workflow\":\"party_auction_v1\",\"next\":"+std::to_string(j.next)+",\"items\":[";
    for(const auto& i:j.items) {if(out.back()!='[')out+=',';out+=AuctionPostItemJson(i);}
    return out+"]}";
}
inline bool DecodePartyAuctionJob(const std::string& value,PartyAuctionJob& job) {
    job={};if(value.size()>4096)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(value);boost::property_tree::read_json(in,p);
        if(p.get<std::string>("workflow")!="party_auction_v1")return false;
        PartyAuctionJob j;j.next=p.get<uint32_t>("next");
        for(const auto& row:p.get_child("items")) {
            if(!row.first.empty() || j.items.size()>=PartyAuctionBatchLimit)return false;
            const auto& i=row.second;
            j.items.push_back({i.get<uint32_t>("guid"),i.get<uint32_t>("entry"),i.get<uint32_t>("quantity"),
                i.get<uint32_t>("buyout"),i.get<uint32_t>("minutes")});
        }
        if(!ValidPartyAuctionJob(j) || EncodePartyAuctionJob(j)!=value)return false;
        job=std::move(j);return true;
    } catch(...) {return false;}
}
inline bool ValidatePartyAuctionTask(const Task& t,std::string& why) {
    if(t.source!="party_auction")return true;
    PartyAuctionJob j;
    if(!IsPartyAuctionTask(t) || !DecodePartyAuctionJob(t.checkpoint.data,j) ||
        (t.checkpoint.step!="party_auction_prepare" && t.checkpoint.step!="party_auction_post" &&
         t.checkpoint.step!="profession_service_auction") || (t.phase==Phase::Queued && j.next) ||
        ((t.phase==Phase::Completed)!=(j.next==j.items.size()))) {
        why="invalid_party_auction_task";return false;
    }
    return true;
}
inline bool PartyAuctionQuoteMatches(const Task& t,const AuctionPostQuote& q) {
    PartyAuctionJob j;std::string why;
    return IsPartyAuctionTask(t) && ValidatePartyAuctionTask(t,why) && t.actor==q.actor &&
        ValidAuctionPostQuote(q) && DecodePartyAuctionJob(t.checkpoint.data,j) && j.next<j.items.size() &&
        AuctionPostItemJson(j.items[j.next])==AuctionPostItemJson(q.item);
}
inline bool PreservePartyAuctionIntent(const Task& before,const Task& after,std::string& why) {
    if(before.source!="party_auction")return true;
    if(!IsPartyAuctionTask(after) || before.checkpoint.data!=after.checkpoint.data) {
        why="party_auction_intent_requires_native_receipt";return false;
    }
    return true;
}
inline bool AcknowledgePartyAuctionPost(Task& after,const AuctionPostQuote& q,
    const std::vector<ClaimConsumption>& uses,const OperationResult& proof,const AuctionPostReceipt& receipt) {
    if(after.phase!=Phase::Verifying || !PartyAuctionQuoteMatches(after,q) ||
        !ExactAuctionPostConsumption(after.root,q,uses) || !VerifyAuctionPost(q,receipt) || !IsUuid(proof.id) ||
        proof.task!=after.id || !after.revision || proof.taskRevision!=after.revision-1 ||
        proof.kind!="party_auction_post" || proof.state!=OperationState::Verified || proof.evidence!="native_auction_post_escrow_and_deposit_observed" ||
        proof.nativeReference!=AuctionPostReference(receipt))return false;
    PartyAuctionJob job;if(!DecodePartyAuctionJob(after.checkpoint.data,job))return false;
    ++job.next;after.checkpoint.data=EncodePartyAuctionJob(job);
    if(job.next==job.items.size())after.phase=Phase::Completed;
    return true;
}
inline bool PartyAuctionReferenceMatches(const std::string& value,uint32_t item) {
    const std::string prefix="auction_post:",suffix=":item:"+std::to_string(item);
    if(value.size()<=prefix.size()+suffix.size() || value.compare(0,prefix.size(),prefix) ||
        value.compare(value.size()-suffix.size(),suffix.size(),suffix))return false;
    const auto number=value.substr(prefix.size(),value.size()-prefix.size()-suffix.size());
    if(number.size()>10 || number[0]=='0' || number.find_first_not_of("0123456789")!=std::string::npos)return false;
    return std::stoull(number)<=UINT32_MAX;
}
}
