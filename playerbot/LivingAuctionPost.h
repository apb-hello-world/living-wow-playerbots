#pragma once
#include "LivingActivityClaimConsumption.h"
#include <boost/property_tree/json_parser.hpp>
#include <sstream>

namespace LivingActivity {
// Values only: never retain an Item/AuctionEntry across a queued operation.
struct AuctionPostItem {
    uint32_t guid=0,entry=0,quantity=0,buyout=0,minutes=0;
};
inline bool ValidAuctionPostItem(const AuctionPostItem& i) {
    return i.guid && i.entry && i.quantity && i.buyout>=2 && i.buyout<=uint32_t(INT32_MAX) &&
        (i.minutes==720 || i.minutes==1440 || i.minutes==2880);
}
inline std::string AuctionPostItemJson(const AuctionPostItem& i) {
    return "{\"guid\":"+std::to_string(i.guid)+",\"entry\":"+std::to_string(i.entry)+
        ",\"quantity\":"+std::to_string(i.quantity)+",\"buyout\":"+std::to_string(i.buyout)+
        ",\"minutes\":"+std::to_string(i.minutes)+'}';
}
struct AuctionPostQuote {
    AuctionPostItem item;
    uint32_t actor=0,house=0,auctioneerEntry=0,money=0,deposit=0,bid=0;
    int32_t property=0;
    uint64_t auctioneer=0;
    uint16_t from=0;
};
inline bool ValidAuctionPostQuote(const AuctionPostQuote& q) {
    return ValidAuctionPostItem(q.item) && q.actor && q.house && q.auctioneerEntry && q.auctioneer &&
        q.money<=uint32_t(INT32_MAX) && q.deposit<=q.money && q.bid &&
        q.bid==uint64_t(q.item.buyout)*95/100;
}
inline std::string EncodeAuctionPostQuote(const AuctionPostQuote& q) {
    return "{\"item\":"+AuctionPostItemJson(q.item)+",\"actor\":"+std::to_string(q.actor)+
        ",\"house\":"+std::to_string(q.house)+",\"auctioneer_entry\":"+std::to_string(q.auctioneerEntry)+
        ",\"money\":"+std::to_string(q.money)+",\"deposit\":"+std::to_string(q.deposit)+
        ",\"bid\":"+std::to_string(q.bid)+",\"property\":"+std::to_string(q.property)+
        ",\"auctioneer\":"+std::to_string(q.auctioneer)+",\"from\":"+std::to_string(q.from)+'}';
}
inline bool DecodeAuctionPostQuote(const std::string& value,AuctionPostQuote& q) {
    q={};if(value.size()>2048)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(value);boost::property_tree::read_json(in,p);
        AuctionPostQuote v;auto& i=v.item;
        i.guid=p.get<uint32_t>("item.guid");i.entry=p.get<uint32_t>("item.entry");
        i.quantity=p.get<uint32_t>("item.quantity");i.buyout=p.get<uint32_t>("item.buyout");
        i.minutes=p.get<uint32_t>("item.minutes");
        v.actor=p.get<uint32_t>("actor");v.house=p.get<uint32_t>("house");
        v.auctioneerEntry=p.get<uint32_t>("auctioneer_entry");v.money=p.get<uint32_t>("money");
        v.deposit=p.get<uint32_t>("deposit");v.bid=p.get<uint32_t>("bid");
        v.property=p.get<int32_t>("property");v.auctioneer=p.get<uint64_t>("auctioneer");v.from=p.get<uint16_t>("from");
        if(!ValidAuctionPostQuote(v) || EncodeAuctionPostQuote(v)!=value)return false;
        q=v;return true;
    } catch(...) {return false;}
}
struct AuctionPostReceipt {
    uint32_t auction=0,actor=0,house=0,guid=0,entry=0,quantity=0,deposit=0,bid=0,buyout=0,money=0;
    int32_t property=0;
    uint64_t expires=0;
    bool bagPresent=false,escrowPresent=false;
};
inline bool VerifyAuctionPost(const AuctionPostQuote& q,const AuctionPostReceipt& r) {
    return ValidAuctionPostQuote(q) && r.auction && r.expires && !r.bagPresent && r.escrowPresent &&
        r.actor==q.actor && r.house==q.house && r.guid==q.item.guid && r.entry==q.item.entry &&
        r.quantity==q.item.quantity && r.property==q.property && r.deposit==q.deposit &&
        r.bid==q.bid && r.buyout==q.item.buyout && r.money==q.money-q.deposit;
}
inline std::string AuctionPostReference(const AuctionPostReceipt& r) {
    return "auction_post:"+std::to_string(r.auction)+":item:"+std::to_string(r.guid);
}
inline std::string EncodeAuctionPostReceipt(const AuctionPostReceipt& r) {
    return "{\"auction\":"+std::to_string(r.auction)+",\"actor\":"+std::to_string(r.actor)+
        ",\"house\":"+std::to_string(r.house)+",\"guid\":"+std::to_string(r.guid)+
        ",\"entry\":"+std::to_string(r.entry)+",\"quantity\":"+std::to_string(r.quantity)+
        ",\"deposit\":"+std::to_string(r.deposit)+",\"bid\":"+std::to_string(r.bid)+
        ",\"buyout\":"+std::to_string(r.buyout)+",\"money\":"+std::to_string(r.money)+
        ",\"property\":"+std::to_string(r.property)+",\"expires\":"+std::to_string(r.expires)+
        ",\"bag_present\":"+(r.bagPresent?"true":"false")+",\"escrow_present\":"+(r.escrowPresent?"true":"false")+'}';
}
inline bool DecodeAuctionPostReceipt(const std::string& value,AuctionPostReceipt& out) {
    out={};if(value.size()>2048)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(value);boost::property_tree::read_json(in,p);
        AuctionPostReceipt r;
#define POST_U32(field) r.field=p.get<uint32_t>(#field)
        POST_U32(auction);POST_U32(actor);POST_U32(house);POST_U32(guid);POST_U32(entry);POST_U32(quantity);
        POST_U32(deposit);POST_U32(bid);POST_U32(buyout);POST_U32(money);
#undef POST_U32
        r.property=p.get<int32_t>("property");r.expires=p.get<uint64_t>("expires");
        r.bagPresent=p.get<bool>("bag_present");r.escrowPresent=p.get<bool>("escrow_present");
        if(EncodeAuctionPostReceipt(r)!=value)return false;
        out=r;return true;
    } catch(...) {return false;}
}
// Consumption releases a BAG reservation into native auction escrow, not item
// destruction. Exact escrow + deposit proof is mandatory in the native adapter.
inline bool ExactAuctionPostConsumption(const std::string& root,const AuctionPostQuote& q,
    const std::vector<ClaimConsumption>& uses) {
    if(!IsUuid(root) || !ValidAuctionPostQuote(q) || uses.size()!=(q.deposit?2u:1u))return false;
    bool item=false,money=false;
    for(const auto& use:uses) {
        const auto& c=use.before;
        if(!ValidResourceClaim(c) || c.task!=root || c.actor!=q.actor || c.state!="held")return false;
        if(c.location=="bags" && !item && c.itemGuid==q.item.guid && c.itemEntry==q.item.entry &&
            c.quantity==q.item.quantity && use.used==c.quantity && !c.copper && !c.nativeReference)item=true;
        else if(q.deposit && c.location=="money" && !money && c.copper==q.deposit && use.used==c.copper &&
            !c.itemGuid && !c.itemEntry && !c.quantity && !c.nativeReference)money=true;
        else return false;
    }
    return item && money==bool(q.deposit);
}
}
