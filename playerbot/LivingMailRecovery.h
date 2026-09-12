#pragma once
#include "LivingNativeMailCollection.h"
#include "LivingProfessionResume.h"
namespace LivingActivity {
struct MailRecoverySnapshot {
    NativeMailQuote unchanged;
    NativeItemStack destination;
};
bool DecodeInterruptedMailQuote(const Task&,const StoredCraftOperation&,NativeMailQuote&,std::string&);
bool ReadUncollectedNativeMail(Player&,const NativeMailQuote&,MailRecoverySnapshot&,std::string&);
// Restored intent + unchanged original attachment and destination + atomic DB
// guards. Records a rejected uncommitted attempt, never a collected attachment.
bool PrepareInterruptedMail(const Task&,const WorldContext&,const ProfessionHistory&,
    const UnsettledClaimBatch&,const MailRecoverySnapshot&,uint64_t,const std::string&,
    ProfessionPreparation&,std::string&);
}
