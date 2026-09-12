#include "LivingActivityMailGain.h"
#include <stdexcept>

namespace LivingActivity {
bool ValidMailGainSpec(const MailGainSpec& s) {
    return s.auction && s.guid && ValidItemGainSpec({s.entry,s.quantity});
}
std::string MailGainSpecJson(const MailGainSpec& s) {
    if(!ValidMailGainSpec(s)) throw std::invalid_argument("Exact auction stack required");
    return "{\"auction\":"+std::to_string(s.auction)+",\"guid\":"+std::to_string(s.guid)+
        ",\"entry\":"+std::to_string(s.entry)+",\"quantity\":"+std::to_string(s.quantity)+'}';
}
bool VerifyNativeMailGain(uint32_t actor,const MailGainSpec& s,const NativeResourceBalance& b,std::string& why) {
    if(!actor || !ValidMailGainSpec(s) || !ValidNativeResourceBalance(b) || b.actor!=actor ||
        b.itemGuid!=s.guid || b.itemEntry!=s.entry || b.quantity!=s.quantity || b.location!="mail") {
        why="auction_native_attachment_mismatch";return false;
    }
    why.clear();return true;
}
ClaimReceiptChange MailGainClaim(const Task& task,const std::string& operation,
    const MailGainSpec& s,const NativeResourceBalance& b) {
    std::string why;
    if(!IsUuid(task.root) || !VerifyNativeMailGain(task.actor,s,b,why))
        throw std::invalid_argument("Native mailed acquisition required");
    ResourceClaim c;c.id=ItemGainClaimId(operation,b.itemGuid);c.task=task.root;c.actor=b.actor;
    c.itemGuid=b.itemGuid;c.itemEntry=b.itemEntry;c.quantity=b.quantity;c.location="mail";
    c.nativeReference=b.nativeReference;c.state="held";
    if(!ValidResourceClaim(c)) throw std::invalid_argument("Invalid mailed acquisition claim");
    return {c,0};
}
ClaimedOutcome MailedOperationWrite(const Task& task,uint64_t expected,const OperationResult& result,
    const std::string& receipt,const std::string& nativeAfter,const std::vector<ClaimConsumption>& use,
    const MailGainSpec& spec,const NativeResourceBalance& b) {
    if(result.kind!="auction_purchase" || result.state!=OperationState::Verified || use.size()!=1 ||
        use.front().before.location!="money" || !use.front().used)
        throw std::invalid_argument("One verified auction payment required");
    const auto change=MailGainClaim(task,result.id,spec,b);const auto& c=change.after;
    const auto state="{\"result\":"+nativeAfter+",\"mail_gain\":"+MailGainSpecJson(spec)+
        ",\"mail\":"+std::to_string(b.nativeReference)+'}';
    auto out=ConsumedOperationWrite(task,expected,result,receipt,state,use);
    out.journal.statements.front()+=" AND EXISTS (SELECT 1 FROM living_activity_operation o WHERE o.operation_id="+
        SqlValue(result.id)+" AND JSON_COMPACT(JSON_EXTRACT(o.before_state,'$.mail_gain'))="+SqlValue(MailGainSpecJson(spec))+')';
    out.journal.statements.front()+=" AND NOT EXISTS (SELECT 1 FROM living_activity_claim c WHERE c.claim_id="+SqlValue(c.id)+')';
    const auto accepted=out.journal.receiptQuery;
    const auto values=SqlValue(c.id)+','+SqlValue(c.task)+','+std::to_string(c.actor)+','+
        std::to_string(c.itemGuid)+','+std::to_string(c.itemEntry)+','+std::to_string(c.quantity)+
        ",0,'mail',"+std::to_string(c.nativeReference)+",'held',1,"+std::to_string(task.updatedAtMs);
    out.journal.statements.push_back("INSERT INTO living_activity_claim (claim_id,task_id,actor_guid,item_guid,item_entry,quantity,"
        "copper,location,native_reference,state,revision,updated_at_ms) SELECT "+values+
        " WHERE EXISTS ("+accepted+") ON DUPLICATE KEY UPDATE claim_id=claim_id");
    out.journal.receiptQuery+=" AND EXISTS (SELECT 1 FROM living_activity_claim c WHERE c.claim_id="+SqlValue(c.id)+
        " AND c.task_id="+SqlValue(c.task)+" AND c.actor_guid="+std::to_string(c.actor)+
        " AND c.item_guid="+std::to_string(c.itemGuid)+" AND c.item_entry="+std::to_string(c.itemEntry)+
        " AND c.quantity="+std::to_string(c.quantity)+" AND c.copper=0 AND c.location='mail' AND c.native_reference="+
        std::to_string(c.nativeReference)+" AND c.state='held' AND c.revision=1)";
    out.changes.push_back(change);return out;
}
}
