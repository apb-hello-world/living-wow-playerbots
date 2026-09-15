#pragma once
#include "LivingCommissionMail.h"
#include "LivingProfessionEvidence.h"
#include "LivingProfessionResume.h"

namespace LivingActivity {
enum class CommissionDeliveryState { WaitingCustomer, WaitingFee, Returned, Complete };
struct CommissionDeliveryProof {
    CommissionDeliveryState state=CommissionDeliveryState::WaitingCustomer;
    CommissionMailQuote quote;
    std::string send,received,fee,returned;
    uint32_t mail=0,paymentMail=0;
};
bool InspectCommissionDelivery(const Task&,const ProfessionHistory&,CommissionDeliveryProof&,std::string&);
// Receipt-guarded metadata only: no new scheduler, native operation or resource
// movement. Empty unsettled claims and exact persisted receipts are required.
bool PrepareCommissionSettlement(const Task&,const WorldContext&,const ProfessionHistory&,
    const UnsettledClaimBatch&,uint64_t,const std::string&,ProfessionPreparation&,std::string&);
}
