#pragma once
#include "LivingGuildDelivery.h"
#include "LivingActivityResources.h"

namespace LivingActivity {
// A quote describes one real stack movement, never an invented contribution.
// Partial stacks are permitted; the shared claim book protects the remainder.
struct GuildDepositQuote {
    GuildDeliveryJob job;
    uint32_t actor=0,item=0,itemCount=0,amount=0,deposited=0,bankCount=0,bagCount=0,money=0;
    uint32_t goalTarget=0,goalReserved=0,goalUpdated=0;
    uint16_t position=0;
    uint8_t tab=0;
    uint64_t bank=0;
};
inline bool ValidGuildDepositQuote(const GuildDepositQuote& q) {
    return ValidGuildDeliveryJob(q.job) && !q.job.money && q.actor && q.item && q.bank &&
        (!q.job.incomingMail ? q.actor==q.job.donor : true) && q.itemCount && q.bagCount>=q.itemCount &&
        q.amount && q.amount<=255 && q.amount<=q.itemCount && q.deposited<q.job.quantity &&
        q.amount<=q.job.quantity-q.deposited && uint64_t(q.bankCount)+q.amount<=UINT32_MAX &&
        q.goalTarget && uint64_t(q.bankCount)+q.amount+q.goalReserved<=q.goalTarget && q.tab<6;
}
inline bool ExactGuildDepositClaim(const GuildDepositQuote& q,const ResourceClaim& claim,const std::string& task) {
    return ValidGuildDepositQuote(q) && ValidResourceClaim(claim) && claim.task==task && claim.actor==q.actor &&
        claim.itemGuid==q.item && claim.itemEntry==q.job.entry && claim.quantity>=q.amount &&
        claim.quantity<=q.job.quantity-q.deposited && !claim.copper && !claim.nativeReference &&
        claim.location=="bags" && claim.state=="held";
}
inline bool VerifyGuildDeposit(const GuildDepositQuote& q,uint32_t sourceAfter,uint32_t bagsAfter,
    uint32_t bankAfter,uint32_t moneyAfter) {
    return ValidGuildDepositQuote(q) && sourceAfter==q.itemCount-q.amount && bagsAfter==q.bagCount-q.amount &&
        uint64_t(bankAfter)==uint64_t(q.bankCount)+q.amount && moneyAfter==q.money;
}
std::string EncodeGuildDepositQuote(const GuildDepositQuote& quote);
bool DecodeGuildDepositQuote(const std::string& text,GuildDepositQuote& quote);
}
