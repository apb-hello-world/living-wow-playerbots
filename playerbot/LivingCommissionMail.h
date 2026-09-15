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

// Post-success native observations. These do not admit work, move possessions,
// change task state, or substitute for the original verified send operation.
enum class CommissionMailEvent { CustomerReceived, FeeCollected, ParcelReturned };
struct CommissionMailObservation {
    CommissionMailEvent event=CommissionMailEvent::CustomerReceived;
    std::string sendOperation;
    uint32_t mail=0,sender=0,receiver=0,item=0,entry=0,quantity=0,copper=0;
    uint32_t moneyBefore=0,moneyAfter=0;
    uint32_t inventoryBefore=0,inventoryAfter=0;
    AuctionMail generated; // Exact native COD payment or return envelope.
    uint64_t atMs=0;
};
std::string CommissionMailOperationFromSubject(const std::string&);
std::string CommissionMailReceiptId(const std::string&,CommissionMailEvent);
// One idempotent INSERT SELECT, executed inside the native effect transaction.
// An unmatched/fabricated subject cannot create evidence. Receipt/payment are
// retained even after native envelopes are deleted or collected after restart.
std::string CommissionMailObservationWrite(const CommissionMailObservation&);
}
