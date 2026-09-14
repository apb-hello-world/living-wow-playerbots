#pragma once
#include "LivingCommissionJob.h"
#include "LivingActivityClaimConsumption.h"
#include "LivingAuctionCapture.h"

namespace LivingActivity {
// A send receipt is proof of custody in native mail, not delivery or payment.
struct CommissionMailQuote {
    std::string commission;
    uint32_t sender=0,receiver=0,item=0,entry=0,quantity=0,count=0;
    uint32_t moneyBefore=0,postage=0,cod=0,delay=0;
    uint16_t position=0;
    uint64_t mailbox=0;
};
bool ValidCommissionMailQuote(const CommissionMailQuote&);
std::string EncodeCommissionMailQuote(const CommissionMailQuote&);
bool DecodeCommissionMailQuote(const std::string&,CommissionMailQuote&);
bool ExactCommissionMailConsumption(const Task&,const CommissionMailQuote&,const std::vector<ClaimConsumption>&);
std::string CommissionMailSubject(const std::string& operation);
bool VerifyCommissionMailSent(const CommissionMailQuote&,const AuctionMail&,const std::string& operation);
std::string CommissionMailSentProof(const Task&,const CommissionMailQuote&,const AuctionMail&,const std::string& operation);
}
