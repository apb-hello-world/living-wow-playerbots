#include "LivingCommissionMail.h"
#include <boost/uuid/name_generator.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace LivingActivity {
bool ValidCommissionMailQuote(const CommissionMailQuote& q) {
    return q.commission.size()>=5 && q.commission.size()<=36 && q.commission.compare(0,4,"lwc-")==0 &&
        q.commission.find_first_not_of("0123456789",4)==std::string::npos && q.sender && q.receiver &&
        q.sender!=q.receiver && q.item && q.entry && q.quantity && q.quantity<=10000 && q.count==q.quantity &&
        q.postage==30 && q.moneyBefore>=q.postage && q.mailbox && q.delay<=30u*86400u;
}
std::string EncodeCommissionMailQuote(const CommissionMailQuote& q) {
    if(!ValidCommissionMailQuote(q))throw std::invalid_argument("exact_commission_mail_quote_required");
    boost::property_tree::ptree p;p.put("version",1);p.put("commission",q.commission);
    p.put("sender",q.sender);p.put("receiver",q.receiver);p.put("item",q.item);p.put("entry",q.entry);
    p.put("quantity",q.quantity);p.put("count",q.count);p.put("money_before",q.moneyBefore);
    p.put("postage",q.postage);p.put("cod",q.cod);p.put("delay",q.delay);p.put("position",q.position);p.put("mailbox",q.mailbox);
    std::ostringstream out;boost::property_tree::write_json(out,p,false);return out.str();
}
bool DecodeCommissionMailQuote(const std::string& text,CommissionMailQuote& out) {
    out={};if(text.empty() || text.size()>2048)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(text);boost::property_tree::read_json(in,p);
        CommissionMailQuote q;q.commission=p.get<std::string>("commission");
        q.sender=p.get<uint32_t>("sender");q.receiver=p.get<uint32_t>("receiver");q.item=p.get<uint32_t>("item");
        q.entry=p.get<uint32_t>("entry");q.quantity=p.get<uint32_t>("quantity");q.count=p.get<uint32_t>("count");
        q.moneyBefore=p.get<uint32_t>("money_before");q.postage=p.get<uint32_t>("postage");q.cod=p.get<uint32_t>("cod");
        q.delay=p.get<uint32_t>("delay");q.position=p.get<uint16_t>("position");q.mailbox=p.get<uint64_t>("mailbox");
        if(!ValidCommissionMailQuote(q) || EncodeCommissionMailQuote(q)!=text)return false;
        out=std::move(q);return true;
    } catch(const std::exception&) {return false;}
}
bool ExactCommissionMailConsumption(const Task& task,const CommissionMailQuote& q,const std::vector<ClaimConsumption>& uses) {
    CommissionJob job;ProfessionJob recipe;std::string why;
    if(!ValidCommissionMailQuote(q) || !ValidateCommissionTask(task,why) || !IsCommissionJob(task) ||
        !DecodeCommissionJob(task.checkpoint.data,job,why) || !job.craftFinishedRevision ||
        !DecodeProfessionIntent(job.craft,recipe,why) || task.actor!=q.sender || job.agreement.id!=q.commission ||
        job.agreement.recipient!=q.receiver || job.agreement.delivery!="mail" || job.agreement.feeCopper!=q.cod ||
        recipe.outputEntry!=q.entry || recipe.outputQuantity!=q.quantity || uses.size()<2 || uses.size()>16)return false;
    uint64_t items=0;bool money=false;std::set<std::string> ids;
    for(const auto& use:uses) {
        const auto& c=use.before;
        if(!ValidResourceClaim(c) || c.task!=task.id || c.actor!=q.sender || c.state!="held" ||
            c.nativeReference || !ids.insert(c.id).second)return false;
        if(c.location=="bags" && c.itemGuid==q.item && c.itemEntry==q.entry && !c.copper &&
            c.quantity && c.quantity<=q.quantity && use.used==c.quantity) {
            items+=c.quantity;if(items>q.quantity)return false;
        }
        else if(c.location=="money" && !c.itemGuid && !c.itemEntry && !c.quantity &&
            c.copper==q.postage && use.used==q.postage && !money)money=true;
        else return false;
    }
    return items==q.quantity && money;
}
std::string CommissionMailSubject(const std::string& operation) {
    if(!IsUuid(operation))throw std::invalid_argument("commission_mail_operation_required");
    return "Commission delivery "+operation;
}
bool VerifyCommissionMailSent(const CommissionMailQuote& q,const AuctionMail& m,const std::string& operation) {
    return ValidCommissionMailQuote(q) && IsUuid(operation) && m.id && m.sender==q.sender && m.receiver==q.receiver &&
        !m.money && m.cod==q.cod && m.attachments==1 && m.itemGuid==q.item && m.itemEntry==q.entry &&
        m.quantity==q.quantity && m.deliveredAt && m.expiresAt>m.deliveredAt &&
        m.subject==CommissionMailSubject(operation);
}
std::string CommissionMailSentProof(const Task& task,const CommissionMailQuote& q,const AuctionMail& m,const std::string& operation) {
    if(task.actor!=q.sender || !VerifyCommissionMailSent(q,m,operation))return {};
    const auto n=[](uint64_t v){return std::to_string(v);};
    return "SELECT "+SqlValue(task.id)+','+n(task.revision)+" FROM mail m JOIN mail_items mi ON mi.mail_id=m.id"
        " JOIN item_instance i ON i.guid=mi.item_guid WHERE m.id="+n(m.id)+
        " AND m.messageType=0 AND m.sender="+n(q.sender)+" AND m.receiver="+n(q.receiver)+
        " AND m.money=0 AND m.cod="+n(q.cod)+" AND m.subject="+SqlValue(m.subject)+
        " AND m.deliver_time="+n(m.deliveredAt)+" AND m.expire_time="+n(m.expiresAt)+
        " AND mi.item_guid="+n(q.item)+" AND mi.item_template="+n(q.entry)+" AND mi.receiver="+n(q.receiver)+
        " AND i.owner_guid="+n(q.receiver)+" AND i.itemEntry="+n(q.entry)+" AND i.count="+n(q.quantity)+
        " AND (SELECT COUNT(*) FROM mail_items a WHERE a.mail_id=m.id)=1"
        " AND NOT EXISTS (SELECT 1 FROM character_inventory v WHERE v.item=i.guid)"
        " AND NOT EXISTS (SELECT 1 FROM guild_bank_item g WHERE g.item_guid=i.guid)"
        " AND NOT EXISTS (SELECT 1 FROM mail_items a WHERE a.item_guid=i.guid AND a.mail_id<>m.id)"
        " AND EXISTS (SELECT 1 FROM characters c WHERE c.guid="+n(q.sender)+" AND c.money="+n(q.moneyBefore-q.postage)+')';
}
std::string CommissionMailOperationFromSubject(const std::string& subject) {
    const std::string prefix="Commission delivery ";
    if(subject.size()!=prefix.size()+36 || subject.compare(0,prefix.size(),prefix))return {};
    const auto id=subject.substr(prefix.size());return IsUuid(id)?id:std::string();
}
namespace {
const char* ReceiptKind(CommissionMailEvent event) {
    switch(event) {
        case CommissionMailEvent::CustomerReceived:return "commission_customer_received";
        case CommissionMailEvent::FeeCollected:return "commission_fee_collected";
        case CommissionMailEvent::ParcelReturned:return "commission_parcel_returned";
    }
    return "";
}
}
std::string CommissionMailReceiptId(const std::string& send,CommissionMailEvent event) {
    if(!IsUuid(send) || !*ReceiptKind(event))throw std::invalid_argument("commission_receipt_identity_required");
    static const auto ns=boost::uuids::string_generator()("9c7ca6fa-e2fd-5b13-a2ab-b912d016f742");
    return boost::uuids::to_string(boost::uuids::name_generator(ns)(send+':'+ReceiptKind(event)));
}
std::string CommissionMailObservationWrite(const CommissionMailObservation& e) {
    if(!IsUuid(e.sendOperation) || !*ReceiptKind(e.event) || !e.mail || !e.sender || !e.receiver ||
        e.sender==e.receiver || !e.atMs)return {};
    const auto n=[](uint64_t v){return std::to_string(v);};
    const auto value=[](const char* field){return std::string("JSON_UNQUOTE(JSON_EXTRACT(o.before_state,'$.native.native.")+field+"'))";};
    const auto subject=CommissionMailSubject(e.sendOperation);
    const auto receipt=CommissionMailReceiptId(e.sendOperation,e.event);
    const auto receiveId=CommissionMailReceiptId(e.sendOperation,CommissionMailEvent::CustomerReceived);
    const auto returnId=CommissionMailReceiptId(e.sendOperation,CommissionMailEvent::ParcelReturned);
    std::string guard="o.operation_id="+SqlValue(e.sendOperation)+" AND o.kind='commission_mail_send' AND o.state='verified'"
        " AND o.evidence_code='native_commission_parcel_and_postage_observed' AND t.source='commission_job'"
        " AND t.actor_guid="+value("sender")+" AND t.source_key="+value("commission")+
        " AND o.native_reference=CONCAT('mail:',JSON_UNQUOTE(JSON_EXTRACT(o.after_state,'$.native.mail')),':item:',"+value("item")+')';
    // Receipt hooks record completed native acts even if admission has since
    // stopped or the task was cancelled. Cancellation cannot erase custody.
    std::string nativeGuard;
    const auto& m=e.generated;
    if(e.event==CommissionMailEvent::CustomerReceived) {
        if(!e.item || !e.entry || !e.quantity || e.quantity>10000 || e.moneyBefore<e.copper || e.moneyAfter!=e.moneyBefore-e.copper ||
            uint64_t(e.inventoryBefore)+e.quantity!=e.inventoryAfter)return {};
        if(e.copper ? !m.id || m.sender!=e.receiver || m.receiver!=e.sender || m.money!=e.copper || m.cod ||
            m.attachments || m.itemGuid || m.itemEntry || m.quantity || m.subject!=subject ||
            !m.deliveredAt || m.expiresAt<=m.deliveredAt : m.id!=0)return {};
        guard+=" AND "+value("sender")+'='+n(e.sender)+" AND "+value("receiver")+'='+n(e.receiver)+
            " AND "+value("item")+'='+n(e.item)+" AND "+value("entry")+'='+n(e.entry)+
            " AND "+value("quantity")+'='+n(e.quantity)+" AND "+value("cod")+'='+n(e.copper)+
            " AND o.native_reference="+SqlValue("mail:"+n(e.mail)+":item:"+n(e.item))+
            " AND NOT EXISTS(SELECT 1 FROM living_activity_operation r WHERE r.operation_id="+SqlValue(returnId)+')';
        nativeGuard=" AND EXISTS(SELECT 1 FROM mail a WHERE a.id="+n(e.mail)+" AND a.messageType=0 AND a.sender="+
            n(e.sender)+" AND a.receiver="+n(e.receiver)+" AND a.cod=0 AND a.money=0 AND a.subject="+SqlValue(subject)+')'+
            " AND NOT EXISTS(SELECT 1 FROM mail_items a WHERE a.mail_id="+n(e.mail)+')'+
            " AND EXISTS(SELECT 1 FROM characters c WHERE c.guid="+n(e.receiver)+" AND c.money="+n(e.moneyAfter)+')';
        if(e.copper)nativeGuard+=" AND EXISTS(SELECT 1 FROM mail a WHERE a.id="+n(m.id)+" AND a.messageType=0 AND a.sender="+
            n(m.sender)+" AND a.receiver="+n(m.receiver)+" AND a.money="+n(m.money)+
            " AND a.cod=0 AND (a.checked & 8)=8 AND a.subject="+SqlValue(subject)+
            " AND a.deliver_time="+n(m.deliveredAt)+" AND a.expire_time="+n(m.expiresAt)+')'+
            " AND NOT EXISTS(SELECT 1 FROM mail_items a WHERE a.mail_id="+n(m.id)+')';
    } else if(e.event==CommissionMailEvent::FeeCollected) {
        if(!e.copper || e.item || e.entry || e.quantity || m.id ||
            uint64_t(e.moneyBefore)+e.copper!=e.moneyAfter)return {};
        guard+=" AND "+value("sender")+'='+n(e.receiver)+" AND "+value("receiver")+'='+n(e.sender)+
            " AND "+value("cod")+'='+n(e.copper)+
            " AND EXISTS(SELECT 1 FROM living_activity_operation r WHERE r.operation_id="+SqlValue(receiveId)+
            " AND r.task_id=o.task_id AND r.kind='commission_customer_received' AND r.state='verified'"
            " AND JSON_UNQUOTE(JSON_EXTRACT(r.after_state,'$.payment_mail'))="+n(e.mail)+')';
        nativeGuard=" AND EXISTS(SELECT 1 FROM mail a WHERE a.id="+n(e.mail)+" AND a.messageType=0 AND a.sender="+
            n(e.sender)+" AND a.receiver="+n(e.receiver)+" AND a.money=0 AND a.cod=0 AND (a.checked & 8)=8 AND a.subject="+
            SqlValue(subject)+") AND EXISTS(SELECT 1 FROM characters c WHERE c.guid="+n(e.receiver)+" AND c.money="+n(e.moneyAfter)+')';
    } else {
        if(!e.item || !e.entry || !e.quantity || e.quantity>10000 || e.copper || m.id!=e.mail || m.sender!=e.sender || m.receiver!=e.receiver ||
            m.money || m.cod || m.attachments!=1 || m.itemGuid!=e.item || m.itemEntry!=e.entry ||
            m.quantity!=e.quantity || m.subject!=subject || !m.deliveredAt || m.expiresAt<=m.deliveredAt)return {};
        guard+=" AND "+value("sender")+'='+n(e.receiver)+" AND "+value("receiver")+'='+n(e.sender)+
            " AND "+value("item")+'='+n(e.item)+" AND "+value("entry")+'='+n(e.entry)+
            " AND "+value("quantity")+'='+n(e.quantity)+
            " AND NOT EXISTS(SELECT 1 FROM living_activity_operation r WHERE r.operation_id="+SqlValue(receiveId)+')';
        nativeGuard=" AND EXISTS(SELECT 1 FROM mail a JOIN mail_items mi ON mi.mail_id=a.id JOIN item_instance i ON i.guid=mi.item_guid"
            " WHERE a.id="+n(e.mail)+" AND a.messageType=0 AND a.sender="+n(e.sender)+" AND a.receiver="+n(e.receiver)+
            " AND a.money=0 AND a.cod=0 AND (a.checked & 2)=2 AND a.subject="+SqlValue(subject)+
            " AND a.deliver_time="+n(m.deliveredAt)+" AND a.expire_time="+n(m.expiresAt)+
            " AND mi.item_guid="+n(e.item)+" AND mi.receiver="+n(e.receiver)+" AND mi.item_template="+n(e.entry)+
            " AND i.owner_guid="+n(e.receiver)+" AND i.itemEntry="+n(e.entry)+" AND i.count="+n(e.quantity)+')'+
            " AND (SELECT COUNT(*) FROM mail_items a WHERE a.mail_id="+n(e.mail)+")=1"
            " AND NOT EXISTS(SELECT 1 FROM mail_items a WHERE a.item_guid="+n(e.item)+" AND a.mail_id<>"+n(e.mail)+')'+
            " AND NOT EXISTS(SELECT 1 FROM character_inventory a WHERE a.item="+n(e.item)+')'+
            " AND NOT EXISTS(SELECT 1 FROM guild_bank_item a WHERE a.item_guid="+n(e.item)+')';
    }
    const std::string after="{\"version\":1,\"send_operation\":\""+e.sendOperation+"\",\"mail\":"+n(e.mail)+
        ",\"sender\":"+n(e.sender)+",\"receiver\":"+n(e.receiver)+",\"item\":"+n(e.item)+",\"entry\":"+n(e.entry)+
        ",\"quantity\":"+n(e.quantity)+",\"copper\":"+n(e.copper)+",\"money_before\":"+n(e.moneyBefore)+
        ",\"money_after\":"+n(e.moneyAfter)+",\"inventory_before\":"+n(e.inventoryBefore)+
        ",\"inventory_after\":"+n(e.inventoryAfter)+",\"payment_mail\":"+
        n(e.event==CommissionMailEvent::CustomerReceived?m.id:0)+",\"observed_at_ms\":"+n(e.atMs)+'}';
    return "INSERT INTO living_activity_operation(operation_id,task_id,task_revision,kind,request_hash,state,native_reference,"
        "before_state,after_state,evidence_code,created_at_ms,updated_at_ms) SELECT "+SqlValue(receipt)+
        ",o.task_id,t.revision,"+SqlValue(ReceiptKind(e.event))+",SHA2("+SqlValue(after)+",256),'verified',"+
        SqlValue("mail:"+n(e.mail))+",'{}',"+SqlValue(after)+",'native_mail_transaction_observed',"+n(e.atMs)+','+n(e.atMs)+
        " FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id WHERE "+guard+nativeGuard+
        " ON DUPLICATE KEY UPDATE operation_id=VALUES(operation_id)";
}
}
