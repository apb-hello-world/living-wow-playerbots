#pragma once
#include "LivingActivityClaimConsumption.h"
#include <stdexcept>
namespace LivingActivity {
// A whole native stack moves bank/mail -> bags. A merge may replace its GUID
// only after the native adapter verifies both identities and the exact delta.
// Pending transferred protection bridges the native effect and saved receipt.
inline bool ValidBankTransfer(const ResourceClaim& c) {
    return ValidResourceClaim(c) && c.state=="held" && c.location=="bank" &&
        c.itemGuid && c.quantity && !c.copper && !c.nativeReference;
}
inline bool ValidMailTransfer(const ResourceClaim& c) {
    return ValidResourceClaim(c) && c.state=="held" && c.location=="mail" &&
        c.itemGuid && c.quantity && !c.copper && c.nativeReference && c.nativeReference<=UINT32_MAX;
}
inline bool ValidItemTransfer(const ResourceClaim& c) {return ValidBankTransfer(c) || ValidMailTransfer(c);}
inline const char* ItemTransferKind(const ResourceClaim& c) {return c.location=="mail" ? "mail_collect" : "bank_withdraw";}
inline std::string BankTransferIdentity(const ResourceClaim& c) {
    if (!ValidBankTransfer(c)) throw std::invalid_argument("Exact bank transfer claim required");
    return "{\"claim\":\""+c.id+"\",\"revision\":"+std::to_string(c.revision)+
        ",\"guid\":"+std::to_string(c.itemGuid)+",\"entry\":"+std::to_string(c.itemEntry)+
        ",\"quantity\":"+std::to_string(c.quantity)+'}';
}
inline std::string ItemTransferIdentity(const ResourceClaim& c) {
    if (ValidBankTransfer(c)) return BankTransferIdentity(c); // Existing receipt fingerprints stay exact.
    if (!ValidMailTransfer(c)) throw std::invalid_argument("Exact native transfer claim required");
    auto bank=c;bank.location="bank";bank.nativeReference=0;
    const auto identity=BankTransferIdentity(bank);
    return identity.substr(0,identity.size()-1)+",\"mail\":"+std::to_string(c.nativeReference)+'}';
}
inline std::string TransferClaimPredicate(const ResourceClaim& c) {
    return "c.claim_id="+SqlValue(c.id)+" AND c.task_id="+SqlValue(c.task)+" AND c.actor_guid="+std::to_string(c.actor)+
        " AND c.item_guid="+std::to_string(c.itemGuid)+" AND c.item_entry="+std::to_string(c.itemEntry)+
        " AND c.quantity="+std::to_string(c.quantity)+" AND c.copper=0 AND c.native_reference="+std::to_string(c.nativeReference)+" AND c.state='held'"
        " AND c.location="+SqlValue(c.location)+" AND c.revision="+std::to_string(c.revision);
}
inline ClaimedOutcome ItemTransferWrite(const Task& task,uint64_t expected,const OperationResult& result,
    const std::string& receipt,const std::string& nativeAfter,const ResourceClaim& before,uint32_t survivingGuid=0) {
    if (!ValidItemTransfer(before) || before.task!=task.root || before.actor!=task.actor ||
        result.kind!=ItemTransferKind(before) || result.state!=OperationState::Verified || task.phase!=Phase::Verifying)
        throw std::invalid_argument("Verified same-root bank transfer required");
    auto after=before;++after.revision;after.location="bags";after.nativeReference=0;
    if (survivingGuid) after.itemGuid=survivingGuid;
    ClaimedOutcome out;
    out.journal=OperationOutcomeWrite(task,expected,result,receipt,"{\"native\":"+nativeAfter+'}');
    out.journal.statements.front()+=" AND EXISTS (SELECT 1 FROM living_activity_claim c WHERE "+TransferClaimPredicate(before)+')';
    out.journal.statements.front()+=" AND EXISTS (SELECT 1 FROM living_activity_operation o WHERE o.operation_id="+
        SqlValue(result.id)+" AND JSON_COMPACT(JSON_EXTRACT(o.before_state,'$.native.transfer'))="+SqlValue(ItemTransferIdentity(before))+')';
    if (after.itemGuid!=before.itemGuid) {
        // Bind the remapped identity to the destination quoted BEFORE the
        // effect. A later matching stack or aggregate count is not permission.
        out.journal.statements.front()+=" AND EXISTS (SELECT 1 FROM living_activity_operation o WHERE o.operation_id="+
            SqlValue(result.id)+" AND JSON_EXTRACT(o.before_state,'$.native.native.merge_guid')="+
            std::to_string(after.itemGuid)+" AND JSON_EXTRACT(o.before_state,'$.native.native.merge_count')>0)";
    }
    const auto accepted=out.journal.receiptQuery;
    const auto identityUpdate=after.itemGuid==before.itemGuid ? std::string() :
        "c.item_guid="+std::to_string(after.itemGuid)+',';
    out.journal.statements.push_back("UPDATE living_activity_claim c SET "+identityUpdate+"c.location='bags',c.native_reference=0,c.revision="+
        std::to_string(after.revision)+",c.updated_at_ms="+std::to_string(task.updatedAtMs)+" WHERE "+
        TransferClaimPredicate(before)+" AND EXISTS ("+accepted+')');
    out.journal.receiptQuery+=" AND EXISTS (SELECT 1 FROM living_activity_claim c WHERE "+TransferClaimPredicate(after)+')';
    out.changes.push_back({after,before.revision});return out;
}
inline ClaimedOutcome BankTransferWrite(const Task& task,uint64_t expected,const OperationResult& result,
    const std::string& receipt,const std::string& nativeAfter,const ResourceClaim& before) {
    if (!ValidBankTransfer(before)) throw std::invalid_argument("Exact bank claim required");
    return ItemTransferWrite(task,expected,result,receipt,nativeAfter,before);
}
}
