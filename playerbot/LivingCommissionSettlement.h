#pragma once
#include "LivingCommissionMail.h"
#include "LivingProfessionEvidence.h"
#include "LivingProfessionResume.h"
#include "LivingPersonalResourceSettlement.h"

namespace LivingActivity {
enum class CommissionDeliveryState { WaitingCustomer, WaitingFee, Returned, Complete };
struct CommissionDeliveryProof {
    CommissionDeliveryState state=CommissionDeliveryState::WaitingCustomer;
    CommissionMailQuote quote;
    std::string send,received,fee,returned;
    uint32_t mail=0,paymentMail=0,returnedMail=0;
    std::vector<CommissionMailAttachment> receivedAttachments,returnedAttachments;
};
bool InspectCommissionParcelReceipts(CommissionDeliveryProof&,const ProfessionHistory&,const StoredCraftOperation&,std::string&);
bool InspectCommissionDelivery(const Task&,const ProfessionHistory&,CommissionDeliveryProof&,std::string&);
bool ReturnedCommissionClaim(const Task&,const ProfessionHistory&,ResourceClaim&,std::string&);
bool ReturnedCommissionClaims(const Task&,const ProfessionHistory&,std::vector<ResourceClaim>&,std::string&);
std::string ReturnedCommissionClaimGuard(const Task&,const ProfessionHistory&,const ResourceClaim&);
struct CommissionReturnClosure {
    Task task;
    WritePlan plan;
    std::vector<ClaimReceiptChange> claims;
};
// Shared personal-claim bookkeeping before delivery. Ordered output and its
// postage stay reserved; safely owned auxiliary stock is released, not sold or
// moved. Also rebinds a restored pre-send task after native custody checks.
bool PrepareCommissionParcelClaims(const Task&,const WorldContext&,const UnsettledClaimBatch&,
    const std::vector<NativeResourceBalance>&,uint64_t,const std::string&,CommissionReturnClosure&,std::string&);
// Closes unsuccessful delivery only after a native collection receipt and
// current personal custody agree. Releases reservations, never possessions.
bool PrepareCommissionReturnClosure(const Task&,const WorldContext&,const ProfessionHistory&,
    const UnsettledClaimBatch&,const std::vector<NativeResourceBalance>&,uint64_t,
    const std::string&,CommissionReturnClosure&,std::string&);
bool PrepareCommissionReturnResume(const Task&,const WorldContext&,const ProfessionHistory&,
    const UnsettledClaimBatch&,const std::vector<NativeResourceBalance>&,uint64_t,
    const std::string&,ProfessionPreparation&,std::string&);
// Receipt-guarded metadata only: no new scheduler, native operation or resource
// movement. Empty unsettled claims and exact persisted receipts are required.
bool PrepareCommissionSettlement(const Task&,const WorldContext&,const ProfessionHistory&,
    const UnsettledClaimBatch&,uint64_t,const std::string&,ProfessionPreparation&,std::string&);
// A persisted atomic-send intent may be retried only after proving its original
// item, money and claims unchanged and that no matching envelope exists.
bool DecodeUnsentCommission(const Task&,const ProfessionHistory&,const UnsettledClaimBatch&,
    CommissionMailQuote&,std::string&);
bool DecodeInterruptedCommission(const Task&,const ProfessionHistory&,const UnsettledClaimBatch&,
    CommissionMailQuote&,AuctionMail&,std::string&);
struct CommissionSendRecovery {
    Task task;
    WritePlan plan;
    std::vector<ClaimReceiptChange> claims;
};
bool PrepareCapturedCommissionSend(const Task&,const WorldContext&,const ProfessionHistory&,
    const UnsettledClaimBatch&,uint64_t,const std::string&,CommissionSendRecovery&,std::string&);
bool PrepareUnsentCommission(const Task&,const WorldContext&,const ProfessionHistory&,
    const UnsettledClaimBatch&,const CommissionMailQuote&,uint32_t,uint64_t,const std::string&,
    ProfessionPreparation&,std::string&);
}
