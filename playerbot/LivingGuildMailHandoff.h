#pragma once
#include "LivingGuildDelivery.h"
#include "LivingActivityClaimConsumption.h"
#include "LivingActivityItemGain.h"

namespace LivingActivity {
// One exact parcel and native postage, quoted before sending. A larger source
// may be split into a validated empty bag slot inside the SAME native operation.
// This is neither an auction gain nor a same-actor bank/mail transfer.
struct GuildMailQuote {
    GuildDeliveryJob job;
    uint32_t sender=0,receiver=0,item=0,moneyBefore=0,postage=0,delay=0;
    uint32_t bagBefore=0,totalBefore=0;
    uint16_t position=0;
    uint64_t mailbox=0;
    uint32_t sourceCount=0; // Zero preserves the original whole-stack wire format.
    uint16_t splitPosition=0;
};
inline uint32_t GuildMailSourceCount(const GuildMailQuote& q) {return q.sourceCount?q.sourceCount:q.job.quantity;}
bool ValidGuildMailQuote(const GuildMailQuote&);
std::string EncodeGuildMailQuote(const GuildMailQuote&);
bool DecodeGuildMailQuote(const std::string&,GuildMailQuote&);
bool ExactGuildMailConsumption(const Task&,const GuildMailQuote&,const std::vector<ClaimConsumption>&);
bool VerifyGuildMailAttachment(const GuildMailQuote&,const NativeResourceBalance&);
GuildDeliveryJob GuildMailRecipientJob(const GuildMailQuote&,uint32_t nativeMail);
struct GuildMailHandoff {
    WritePlan journal;
    std::vector<ClaimReceiptChange> changes;
    ResourceClaim recipientClaim;
    Task recipient;
};
// Caller supplies the deterministic source ID and the recipient's fresh native
// context. A single retained transaction saves the send outcome, both task
// legs and their claims. No source claim is released before that receipt.
GuildMailHandoff GuildMailHandoffWrite(const Task& sender,uint64_t expected,
    const OperationResult&,const std::string& receipt,const std::string& nativeAfter,
    const GuildMailQuote&,const std::vector<ClaimConsumption>&,
    const NativeResourceBalance& attachment,const Task& recipient,const std::string& recipientReceipt);
// Exact native mail, possession, wallet, goal and delivery proof. Also used by
// database fault tests; a plausible after-state JSON alone is never sufficient.
std::string GuildMailNativeProof(const GuildMailQuote&,const Task& outcome,
    const NativeResourceBalance& attachment,uint64_t deliveredAt,uint64_t expiresAt);
struct GuildMailRollback {
    OperationResult interrupted;
    std::string before,after;
    GuildMailQuote quote;
    uint32_t attemptedMail=0;
    uint32_t attemptedItem=0;
};
// Restart recovery for an unchanged intent or failed capture. Only exact
// original possessions, postage and claims plus absent native effects permit
// rejection of the old attempt. Ambiguous commits remain held, never resent.
bool DecodeGuildMailRollback(const std::string& stored,GuildMailRollback&);
std::string GuildMailRollbackProjection();
struct GuildMailRollbackWrite { Task task;WritePlan journal; };
bool PrepareGuildMailRollback(const Task&,const WorldContext&,const GuildMailRollback&,
    const std::vector<ClaimConsumption>&,const NativeItemStack&,uint32_t money,uint64_t now,
    const std::string& receipt,GuildMailRollbackWrite&,std::string& blocker);
}
