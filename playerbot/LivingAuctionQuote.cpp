#include "LivingAuctionQuote.h"
#include <boost/property_tree/json_parser.hpp>
#include <limits>
#include <set>
#include <sstream>

namespace LivingActivity {
std::string EncodeNativeAuctionQuote(const NativeAuctionQuote& q) {
    return "{\"actor\":"+std::to_string(q.actor)+",\"seller\":"+std::to_string(q.seller)+",\"house\":"+std::to_string(q.house)+
        ",\"auction\":"+std::to_string(q.auction)+",\"guid\":"+std::to_string(q.guid)+",\"entry\":"+std::to_string(q.entry)+
        ",\"quantity\":"+std::to_string(q.quantity)+",\"copper\":"+std::to_string(q.copper)+",\"money_before\":"+std::to_string(q.moneyBefore)+
        ",\"bidder\":"+std::to_string(q.bidder)+",\"bid\":"+std::to_string(q.bid)+",\"proceeds\":"+std::to_string(q.proceeds)+
        ",\"auctioneer_entry\":"+std::to_string(q.auctioneerEntry)+",\"property\":"+std::to_string(q.property)+
        ",\"auctioneer\":"+std::to_string(q.auctioneer)+",\"expires_at\":"+std::to_string(q.expiresAt)+'}';
}
bool DecodeNativeAuctionQuote(const std::string& value,NativeAuctionQuote& q) {
    q={};
    try {
        boost::property_tree::ptree p;std::istringstream in(value);boost::property_tree::read_json(in,p);
#define Q32(name,key) q.name=p.get<uint32_t>(key)
        Q32(actor,"actor");Q32(seller,"seller");Q32(house,"house");Q32(auction,"auction");Q32(guid,"guid");
        Q32(entry,"entry");Q32(quantity,"quantity");Q32(copper,"copper");Q32(moneyBefore,"money_before");
        Q32(bidder,"bidder");Q32(bid,"bid");Q32(proceeds,"proceeds");Q32(auctioneerEntry,"auctioneer_entry");
#undef Q32
        q.property=p.get<int32_t>("property");q.auctioneer=p.get<uint64_t>("auctioneer");q.expiresAt=p.get<uint64_t>("expires_at");
        return value==EncodeNativeAuctionQuote(q) && q.actor && q.seller && q.seller!=q.actor && q.bidder!=q.actor &&
            q.house && q.auctioneer && q.auctioneerEntry && q.expiresAt && ValidMailGainSpec(q.Stack()) &&
            q.copper && q.copper<=uint32_t(INT32_MAX) && q.copper<=q.moneyBefore && q.bid<q.copper &&
            bool(q.bidder)==bool(q.bid);
    } catch(...) {q={};return false;}
}
std::string AuctionMailJson(const AuctionMail& m) {
    // This adapter accepts only the pinned native numeric subject format.
    if(m.subject.empty() || m.subject.size()>120 || m.subject.find_first_not_of("0123456789:-")!=std::string::npos)
        throw std::invalid_argument("Native numeric auction subject required");
    return "{\"id\":"+std::to_string(m.id)+",\"sender\":"+std::to_string(m.sender)+",\"receiver\":"+std::to_string(m.receiver)+
        ",\"money\":"+std::to_string(m.money)+",\"cod\":"+std::to_string(m.cod)+",\"guid\":"+std::to_string(m.itemGuid)+
        ",\"entry\":"+std::to_string(m.itemEntry)+",\"quantity\":"+std::to_string(m.quantity)+
        ",\"attachments\":"+std::to_string(m.attachments)+",\"delivered_at\":"+std::to_string(m.deliveredAt)+
        ",\"expires_at\":"+std::to_string(m.expiresAt)+",\"subject\":\""+m.subject+"\"}";
}
std::string AuctionMailsJson(const std::vector<AuctionMail>& rows) {
    std::string out="[";
    for(const auto& row:rows) {if(out.size()>1)out+=',';out+=AuctionMailJson(row);}
    return out+']';
}
bool VerifyAuctionMails(const NativeAuctionQuote& q,const std::vector<AuctionMail>& rows,
    NativeResourceBalance& acquired,std::string& blocker) {
    acquired={};auto reject=[&](const char* why){blocker=why;return false;};
    NativeAuctionQuote checked;
    if(!DecodeNativeAuctionQuote(EncodeNativeAuctionQuote(q),checked) || rows.size()!=(q.bidder?4u:3u))
        return reject("auction_native_mail_set_incomplete");
    std::set<uint32_t> ids;bool won=false,sold=false,pending=false,refund=false;
    const auto subject=std::to_string(q.entry)+':'+std::to_string(q.property)+':';
    // Pinned TBC enum values are asserted in the native adapter.
    for(const auto& m:rows) {
        if(!m.id || !ids.insert(m.id).second || m.sender!=q.house || m.cod || !m.deliveredAt || m.expiresAt<=m.deliveredAt)
            return reject("auction_native_mail_identity_mismatch");
        if(m.subject==subject+"1" && m.receiver==q.actor && !won) {
            if(m.money || m.attachments!=1 || m.itemGuid!=q.guid || m.itemEntry!=q.entry || m.quantity!=q.quantity)
                return reject("auction_native_attachment_mismatch");
            acquired={q.actor,q.guid,q.entry,q.quantity,0,"mail",m.id};won=true;continue;
        }
        if(m.attachments || m.itemGuid || m.itemEntry || m.quantity)
            return reject("auction_unexpected_native_attachment");
        if(m.subject==subject+"2" && m.receiver==q.seller && m.money==q.proceeds && !sold) sold=true;
        else if(m.subject==subject+"6" && m.receiver==q.seller && !m.money && !pending) pending=true;
        else if(q.bidder && m.subject==subject+"0" && m.receiver==q.bidder && m.money==q.bid && !refund) refund=true;
        else return reject("auction_native_proceeds_or_refund_mismatch");
    }
    if(!won || !sold || !pending || bool(q.bidder)!=refund) return reject("auction_native_mail_set_incomplete");
    blocker.clear();return true;
}
}
