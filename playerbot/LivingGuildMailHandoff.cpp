#include "LivingGuildMailHandoff.h"
#include "LivingActivityItemGain.h"
#include "LivingActivityTransfer.h"
#include <algorithm>
#include <stdexcept>

namespace LivingActivity {
bool ValidGuildMailQuote(const GuildMailQuote& q) {
    return ValidGuildDeliveryJob(q.job) && !q.job.money && q.sender && q.receiver && q.sender!=q.receiver &&
        (q.job.incomingMail || q.sender==q.job.donor) && q.item && q.mailbox && q.postage==30 &&
        q.moneyBefore>=q.postage && q.bagBefore>=q.job.quantity && q.totalBefore>=q.bagBefore &&
        q.delay<=30u*86400u;
}
std::string EncodeGuildMailQuote(const GuildMailQuote& q) {
    if(!ValidGuildMailQuote(q))throw std::invalid_argument("exact_guild_mail_quote_required");
    return "{\"job\":"+EncodeGuildDeliveryJob(q.job)+",\"sender\":"+std::to_string(q.sender)+
        ",\"receiver\":"+std::to_string(q.receiver)+",\"item\":"+std::to_string(q.item)+
        ",\"money_before\":"+std::to_string(q.moneyBefore)+",\"postage\":"+std::to_string(q.postage)+
        ",\"delay\":"+std::to_string(q.delay)+",\"bag_before\":"+std::to_string(q.bagBefore)+
        ",\"total_before\":"+std::to_string(q.totalBefore)+",\"position\":"+std::to_string(q.position)+
        ",\"mailbox\":"+std::to_string(q.mailbox)+'}';
}
bool DecodeGuildMailQuote(const std::string& text,GuildMailQuote& q) {
    q={};if(text.empty() || text.size()>2048)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(text);boost::property_tree::read_json(in,p);
        GuildMailQuote parsed;std::ostringstream job;boost::property_tree::write_json(job,p.get_child("job"),false);
        std::string why;if(!DecodeGuildDeliveryJob(job.str(),parsed.job,why))return false;
        parsed.sender=p.get<uint32_t>("sender");parsed.receiver=p.get<uint32_t>("receiver");parsed.item=p.get<uint32_t>("item");
        parsed.moneyBefore=p.get<uint32_t>("money_before");parsed.postage=p.get<uint32_t>("postage");parsed.delay=p.get<uint32_t>("delay");
        parsed.bagBefore=p.get<uint32_t>("bag_before");parsed.totalBefore=p.get<uint32_t>("total_before");
        parsed.position=p.get<uint16_t>("position");parsed.mailbox=p.get<uint64_t>("mailbox");
        // Internal quotes are canonical, so duplicates, extra fields, signs,
        // narrowing overflows and alternative numeric spellings are rejected.
        if(!ValidGuildMailQuote(parsed) || EncodeGuildMailQuote(parsed)!=text)return false;
        q=parsed;return true;
    } catch(const std::exception&) {return false;}
}
bool ExactGuildMailConsumption(const Task& task,const GuildMailQuote& q,const std::vector<ClaimConsumption>& uses) {
    std::string why;
    if(!ValidGuildMailQuote(q) || !IsManagedGuildDelivery(task) || !ValidateGuildDeliveryTask(task,why) ||
        task.actor!=q.sender || task.checkpoint.data!=EncodeGuildDeliveryJob(q.job) || uses.size()!=2)return false;
    bool item=false,money=false;std::set<std::string> ids;
    for(const auto& use:uses) {
        const auto& c=use.before;
        if(!ValidResourceClaim(c) || c.task!=task.id || c.actor!=q.sender || c.state!="held" ||
            c.nativeReference || !ids.insert(c.id).second)return false;
        if(c.location=="bags" && !c.copper && c.itemGuid==q.item && c.itemEntry==q.job.entry &&
            c.quantity==q.job.quantity && use.used==c.quantity && !item)item=true;
        else if(c.location=="money" && !c.itemGuid && !c.itemEntry && !c.quantity &&
            c.copper==q.postage && use.used==q.postage && !money)money=true;
        else return false;
    }
    return item && money;
}
bool VerifyGuildMailAttachment(const GuildMailQuote& q,const NativeResourceBalance& b) {
    return ValidGuildMailQuote(q) && ValidNativeResourceBalance(b) && b.actor==q.receiver &&
        b.itemGuid==q.item && b.itemEntry==q.job.entry && b.quantity==q.job.quantity &&
        !b.copper && b.location=="mail" && b.nativeReference && b.nativeReference<=UINT32_MAX &&
        b.nativeReference!=q.job.incomingMail;
}
GuildDeliveryJob GuildMailRecipientJob(const GuildMailQuote& q,uint32_t mail) {
    if(!ValidGuildMailQuote(q) || !mail || mail==q.job.incomingMail)
        throw std::invalid_argument("native_outgoing_mail_required");
    auto job=q.job;job.incomingMail=mail;job.mailSender=q.sender==job.donor?0:q.sender;return job;
}
GuildMailHandoff GuildMailHandoffWrite(const Task& sender,uint64_t expected,const OperationResult& result,
    const std::string& receipt,const std::string& nativeAfter,const GuildMailQuote& q,
    const std::vector<ClaimConsumption>& use,const NativeResourceBalance& attachment,
    const Task& recipient,const std::string& recipientReceipt) {
    std::string why;
    if(!ExactGuildMailConsumption(sender,q,use) || !VerifyGuildMailAttachment(q,attachment) ||
        result.kind!="guild_mail_send" || result.state!=OperationState::Verified ||
        result.evidence!="native_guild_parcel_postage_and_handoff_observed" || sender.phase!=Phase::Verifying ||
        !IsManagedGuildDelivery(recipient) || !ValidateGuildDeliveryTask(recipient,why) || recipient.actor!=q.receiver || recipient.id==sender.id ||
        recipient.phase!=Phase::Queued || recipient.revision!=1 || recipient.ownerGeneration ||
        recipient.checkpoint.step!="guild_mail_prepare" || recipient.checkpoint.data!=
        EncodeGuildDeliveryJob(GuildMailRecipientJob(q,uint32_t(attachment.nativeReference))) ||
        recipient.createdAtMs!=sender.updatedAtMs || recipient.updatedAtMs!=sender.updatedAtMs ||
        recipient.context.boot!=sender.context.boot || recipient.context.policyRevision!=sender.context.policyRevision ||
        receipt==recipientReceipt)
        throw std::invalid_argument("verified_guild_parcel_handoff_required");
    const auto creation=TaskWrite(recipient,0,recipientReceipt,"guild_mail_handoff",result.id);
    boost::property_tree::ptree contract,observed;std::istringstream input(nativeAfter);
    boost::property_tree::read_json(input,observed);contract.add_child("result",observed);
    // Bind every recipient task field and its receipt fingerprint into the
    // sender's outcome. Reusing a receipt cannot create a second recipient leg.
    contract.put("recipient_contract",creation.receiptQuery);
    std::ostringstream encoded;boost::property_tree::write_json(encoded,contract,false);
    auto consumed=ConsumedOperationWrite(sender,expected,result,receipt,encoded.str(),use);
    // Bind to the exact planned receiver/parcel, not a new destination chosen
    // after dispatch. Keep old operation fingerprints byte-for-byte unchanged.
    consumed.journal.statements.front()+=" AND EXISTS (SELECT 1 FROM living_activity_operation o WHERE o.operation_id="+
        SqlValue(result.id)+" AND JSON_COMPACT(JSON_EXTRACT(o.before_state,'$.native.native'))="+SqlValue(EncodeGuildMailQuote(q))+')';
    ResourceClaim c;c.id=ItemGainClaimId(result.id,q.item);c.task=recipient.id;c.actor=q.receiver;
    c.itemGuid=q.item;c.itemEntry=q.job.entry;c.quantity=q.job.quantity;c.location="mail";
    c.nativeReference=attachment.nativeReference;c.state="held";
    consumed.journal.statements.front()+=" AND NOT EXISTS (SELECT 1 FROM living_activity_task t WHERE t.task_id="+
        SqlValue(recipient.id)+" OR (t.source='guild_delivery' AND t.source_key="+SqlValue(recipient.sourceKey)+"))";
    consumed.journal.statements.front()+=" AND NOT EXISTS (SELECT 1 FROM living_activity_claim c WHERE c.claim_id="+SqlValue(c.id)+')';
    const auto senderReceipt=consumed.journal.receiptQuery;
    // All plans execute in ONE transaction and the composite receipt is the
    // commit predicate. A fault after any statement rolls back BOTH legs.
    auto insert=creation.statements.front();
    const auto values=insert.find(") VALUES (");
    const auto suffix=insert.rfind(") ON DUPLICATE KEY UPDATE task_id=task_id");
    if(values==std::string::npos || suffix==std::string::npos || suffix<=values)
        throw std::logic_error("guild_recipient_insert_contract_changed");
    insert=insert.substr(0,values+1)+" SELECT "+insert.substr(values+10,suffix-(values+10))+
        " WHERE EXISTS ("+senderReceipt+") ON DUPLICATE KEY UPDATE task_id=task_id";
    consumed.journal.statements.push_back(insert);
    for(size_t n=1;n<creation.statements.size();++n)consumed.journal.statements.push_back(creation.statements[n]);
    consumed.journal.statements.push_back("INSERT INTO living_activity_claim (claim_id,task_id,actor_guid,item_guid,item_entry,quantity,"
        "copper,location,native_reference,state,revision,updated_at_ms) SELECT "+SqlValue(c.id)+','+SqlValue(c.task)+','+
        std::to_string(c.actor)+','+std::to_string(c.itemGuid)+','+std::to_string(c.itemEntry)+','+std::to_string(c.quantity)+
        ",0,'mail',"+std::to_string(c.nativeReference)+",'held',1,"+std::to_string(sender.updatedAtMs)+
        " WHERE EXISTS ("+senderReceipt+") AND EXISTS ("+creation.receiptQuery+") ON DUPLICATE KEY UPDATE claim_id=claim_id");
    consumed.journal.receiptQuery+=" AND EXISTS ("+creation.receiptQuery+") AND EXISTS (SELECT 1 FROM living_activity_claim c WHERE "+
        TransferClaimPredicate(c)+')';
    GuildMailHandoff out;out.journal=std::move(consumed.journal);out.changes=std::move(consumed.changes);
    out.changes.push_back({c,0});out.recipientClaim=c;out.recipient=recipient;return out;
}
std::string GuildMailNativeProof(const GuildMailQuote& q,const Task& task,const NativeResourceBalance& b,
    uint64_t deliveredAt,uint64_t expiresAt) {
    if(!VerifyGuildMailAttachment(q,b) || task.actor!=q.sender || !deliveredAt || expiresAt<=deliveredAt)return {};
    return "SELECT "+SqlValue(task.id)+','+std::to_string(task.revision)+" FROM mail m JOIN mail_items mi ON mi.mail_id=m.id"
        " JOIN item_instance i ON i.guid=mi.item_guid JOIN guild_society_supply_delivery d ON d.mail_id=m.id"
        " JOIN guild_society_supply_goal g ON g.goal_id=d.goal_id AND g.guild_id=d.guild_id"
        " JOIN guild_society_supply_execution e ON e.guild_id=d.guild_id WHERE m.id="+std::to_string(b.nativeReference)+
        " AND m.messageType=0 AND m.sender="+std::to_string(q.sender)+" AND m.receiver="+std::to_string(q.receiver)+
        " AND m.money=0 AND m.cod=0 AND m.deliver_time="+std::to_string(deliveredAt)+" AND m.expire_time="+std::to_string(expiresAt)+
        " AND mi.item_guid="+std::to_string(q.item)+" AND mi.item_template="+std::to_string(q.job.entry)+
        " AND mi.receiver="+std::to_string(q.receiver)+" AND i.owner_guid="+std::to_string(q.receiver)+
        " AND i.itemEntry="+std::to_string(q.job.entry)+" AND i.count="+std::to_string(q.job.quantity)+
        " AND (SELECT COUNT(*) FROM mail_items a WHERE a.mail_id=m.id)=1"
        " AND NOT EXISTS (SELECT 1 FROM character_inventory v WHERE v.item=i.guid)"
        " AND NOT EXISTS (SELECT 1 FROM mail_items a WHERE a.item_guid=i.guid AND a.mail_id<>m.id)"
        " AND EXISTS (SELECT 1 FROM characters c WHERE c.guid="+std::to_string(q.sender)+
        " AND c.money="+std::to_string(q.moneyBefore-q.postage)+')'+
        " AND d.delivery_id="+std::to_string(q.job.delivery)+" AND d.guild_id="+std::to_string(q.job.guild)+
        " AND d.goal_id="+SqlValue(q.job.goal)+" AND d.donor_guid="+std::to_string(q.job.donor)+
        " AND d.carrier_guid="+std::to_string(q.receiver)+" AND d.item_guid="+std::to_string(q.item)+
        " AND d.item_entry="+std::to_string(q.job.entry)+" AND d.quantity="+std::to_string(q.job.quantity)+
        " AND d.deposited_quantity=0 AND d.phase='mailed' AND g.state='active' AND g.request_kind='item'"
        " AND g.item_entry="+std::to_string(q.job.entry)+" AND e.enabled=1";
}
}
