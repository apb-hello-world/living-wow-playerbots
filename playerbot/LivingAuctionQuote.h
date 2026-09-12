#pragma once
#include "LivingActivityMailGain.h"
#include "LivingAuctionCapture.h"

namespace LivingActivity {
struct NativeAuctionQuote {
    uint32_t actor=0,seller=0,house=0,auction=0,guid=0,entry=0,quantity=0;
    uint32_t copper=0,moneyBefore=0,bidder=0,bid=0,proceeds=0,auctioneerEntry=0;
    int32_t property=0;
    uint64_t auctioneer=0,expiresAt=0;
    MailGainSpec Stack() const {return {auction,guid,entry,quantity};}
};
std::string EncodeNativeAuctionQuote(const NativeAuctionQuote& quote);
bool DecodeNativeAuctionQuote(const std::string& value,NativeAuctionQuote& quote);
std::string AuctionMailJson(const AuctionMail& mail);
bool VerifyAuctionMails(const NativeAuctionQuote& quote,const std::vector<AuctionMail>& mails,
    NativeResourceBalance& acquired,std::string& blocker);
std::string AuctionMailsJson(const std::vector<AuctionMail>& mails);
}
