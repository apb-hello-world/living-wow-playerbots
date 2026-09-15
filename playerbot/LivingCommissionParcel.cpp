#include "LivingCommissionMail.h"
#include "LivingCommissionSettlement.h"
#include <boost/uuid/name_generator.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

namespace LivingActivity {
bool InspectCommissionParcelReceipts(CommissionDeliveryProof& proof,const ProfessionHistory& history,
    const StoredCraftOperation& send,std::string& why) {
    auto require=[](bool ok,const char* code){if(!ok)throw std::invalid_argument(code);};
    try {
        const auto& q=proof.quote;const auto parts=CommissionMailAttachments(q);
        require(!q.additional.empty() && ValidCommissionMailQuote(q),"commission_v2_quote_required");
        std::map<uint32_t,CommissionMailAttachment> expected;for(const auto& part:parts)expected[part.item]=part;
        std::map<uint32_t,std::string,std::greater<uint32_t>> receiptOrder;
        std::set<uint32_t> delivered,returned;uint32_t paid=0,feeMail=0;
        for(const auto& row:history.commissionMail) {
            if(row.receipt.id==send.receipt.id)continue;
            const auto& r=row.receipt;boost::property_tree::ptree p;std::istringstream in(row.afterState);
            boost::property_tree::read_json(in,p);
            const auto number=[&](const char* name){return EnchantCodec::Number(p.get_child(name));};
            const auto before=number("attachments_before"),after=number("attachments_after");
            auto remainder=p.get_child_optional("returned_items");
            auto shape=p;shape.erase("attachments_before");shape.erase("attachments_after");shape.erase("returned_items");
            EnchantCodec::Object(shape,{"version","send_operation","mail","sender","receiver","item","entry","quantity","copper",
                "money_before","money_after","inventory_before","inventory_after","payment_mail","observed_at_ms"});
            const auto stamp=p.get<std::string>("observed_at_ms");
            require(!stamp.empty() && stamp.size()<=20 && stamp.find_first_not_of("0123456789")==std::string::npos && std::stoull(stamp)>0,
                "commission_v2_timestamp_invalid");
            require(r.taskRevision>=send.receipt.taskRevision && row.beforeState=="{}" && r.evidence=="native_mail_transaction_observed" &&
                number("version")==2 && p.get<std::string>("send_operation")==proof.send && number("mail") &&
                r.nativeReference=="mail:"+std::to_string(number("mail")),"commission_v2_receipt_identity_changed");
            const auto copper=number("copper"),item=number("item"),quantity=number("quantity"),payment=number("payment_mail");
            if(r.kind=="commission_customer_received") {
                require(!remainder && expected.count(item) && delivered.insert(item).second && quantity==expected.at(item).quantity &&
                    number("entry")==q.entry && number("mail")==proof.mail && number("sender")==q.sender && number("receiver")==q.receiver &&
                    r.id==CommissionParcelReceiptId(proof.send,item) && before>0 && before<=parts.size() && after+1==before &&
                    receiptOrder.emplace(before,r.id).second && uint64_t(number("inventory_before"))+quantity==number("inventory_after") &&
                    number("money_before")>=copper && number("money_after")==number("money_before")-copper,
                    "commission_v2_customer_item_mismatch");
                // COD is charged once, on the first attachment actually taken.
                require(copper==(before==parts.size()?q.cod:0) && bool(payment)==bool(copper),"commission_v2_cod_charged_once_required");
                if(copper){require(!paid,"commission_v2_duplicate_charge");paid=copper;proof.paymentMail=payment;}
                proof.receivedAttachments.push_back(expected.at(item));
            }else if(r.kind=="commission_fee_collected") {
                require(!remainder && proof.fee.empty() && r.id==CommissionMailReceiptId(proof.send,CommissionMailEvent::FeeCollected) &&
                    q.cod && copper==q.cod && number("sender")==q.receiver && number("receiver")==q.sender &&
                    uint64_t(number("money_before"))+copper==number("money_after") && !item && !number("entry") && !quantity &&
                    !payment && !number("inventory_before") && !number("inventory_after") && !before && !after,"commission_v2_fee_mismatch");
                proof.fee=r.id;feeMail=number("mail");
            }else if(r.kind=="commission_parcel_returned") {
                require(remainder && proof.returned.empty() && r.id==CommissionMailReceiptId(proof.send,CommissionMailEvent::ParcelReturned) &&
                    number("mail")!=proof.mail && number("sender")==q.receiver && number("receiver")==q.sender && !copper && !payment &&
                    !number("money_before") && !number("money_after") && !number("inventory_before") && !number("inventory_after") &&
                    before>0 && before<=parts.size() && after==before && remainder->size()==before && remainder->data().empty(),
                    "commission_v2_return_mismatch");
                for(const auto& field:*remainder) {
                    require(field.first.empty(),"commission_v2_return_array_invalid");EnchantCodec::Object(field.second,{"item","quantity"});
                    const auto guid=EnchantCodec::Number(field.second.get_child("item")),count=EnchantCodec::Number(field.second.get_child("quantity"));
                    require(expected.count(guid) && count==expected.at(guid).quantity && returned.insert(guid).second,"commission_v2_returned_item_changed");
                    proof.returnedAttachments.push_back(expected.at(guid));
                }
                require(before==1 ? item==proof.returnedAttachments.front().item && number("entry")==q.entry &&
                    quantity==proof.returnedAttachments.front().quantity : !item && !number("entry") && !quantity,"commission_v2_return_capture_changed");
                proof.returned=r.id;proof.returnedMail=number("mail");
            }else require(false,"commission_v2_unknown_receipt");
        }
        uint32_t next=uint32_t(parts.size());
        for(const auto& row:receiptOrder)require(row.first==next--,"commission_v2_collection_receipt_gap");
        for(auto item:returned)require(!delivered.count(item),"commission_v2_conflicting_item_custody");
        if(!proof.returned.empty())require(returned.size()+delivered.size()==parts.size(),"commission_v2_return_custody_gap");
        if(!proof.fee.empty())require(paid==q.cod && proof.paymentMail==feeMail,"commission_v2_fee_without_native_charge");
        if(delivered.size()==parts.size())proof.received=receiptOrder.rbegin()->second;
        proof.state=!proof.returned.empty()?CommissionDeliveryState::Returned:proof.received.empty()?CommissionDeliveryState::WaitingCustomer:
            q.cod && proof.fee.empty()?CommissionDeliveryState::WaitingFee:CommissionDeliveryState::Complete;
        why.clear();return true;
    }catch(const std::exception& e){why=e.what();return false;}
}
std::string CommissionParcelReceiptId(const std::string& send,uint32_t item) {
    if(!IsUuid(send) || !item)throw std::invalid_argument("commission_parcel_item_receipt_required");
    static const auto ns=boost::uuids::string_generator()("9c7ca6fa-e2fd-5b13-a2ab-b912d016f742");
    return boost::uuids::to_string(boost::uuids::name_generator(ns)(send+":commission_customer_received:item:"+std::to_string(item)));
}
std::string CommissionParcelObservationWrite(const CommissionMailObservation& e) {
    CommissionMailQuote q;
    if(!DecodeCommissionMailQuote(e.parcelQuote,q) || q.additional.empty() || !IsUuid(e.sendOperation) ||
        !e.mail || !e.atMs || (e.event!=CommissionMailEvent::CustomerReceived &&
        e.event!=CommissionMailEvent::FeeCollected && e.event!=CommissionMailEvent::ParcelReturned))return {};
    const auto n=[](uint64_t v){return std::to_string(v);};
    const auto subject=CommissionMailSubject(e.sendOperation),send=SqlValue(e.sendOperation);
    const auto parts=CommissionMailAttachments(q);
    const auto returning=e.event==CommissionMailEvent::ParcelReturned;
    std::string kind,receipt,guard,nativeGuard;
    const std::string itemReceiptCount="(SELECT COUNT(*) FROM living_activity_operation r WHERE r.task_id=o.task_id"
        " AND r.kind='commission_customer_received' AND r.state='verified'"
        " AND JSON_UNQUOTE(JSON_EXTRACT(r.after_state,'$.send_operation'))="+send+')';
    const std::string chargedReceipt="SELECT 1 FROM living_activity_operation r WHERE r.task_id=o.task_id"
        " AND r.kind='commission_customer_received' AND r.state='verified'"
        " AND JSON_UNQUOTE(JSON_EXTRACT(r.after_state,'$.send_operation'))="+send+
        " AND JSON_EXTRACT(r.after_state,'$.copper')="+n(q.cod);
    guard="o.operation_id="+send+" AND o.kind='commission_mail_send' AND o.state='verified'"
        " AND o.evidence_code='native_commission_parcel_and_postage_observed' AND t.source='commission_job'"
        " AND t.actor_guid="+n(q.sender)+" AND t.source_key="+SqlValue(q.commission)+
        " AND JSON_COMPACT(JSON_EXTRACT(o.before_state,'$.native.native'))=JSON_COMPACT("+SqlValue(e.parcelQuote)+')'+
        " AND o.native_reference=CONCAT('mail:',JSON_UNQUOTE(JSON_EXTRACT(o.after_state,'$.native.mail')),':item:',"+n(q.item)+')';
    const auto& payment=e.generated;
    if(e.event==CommissionMailEvent::CustomerReceived) {
        const auto part=std::find_if(parts.begin(),parts.end(),[&](const auto& p){return p.item==e.item;});
        if(part==parts.end() || e.sender!=q.sender || e.receiver!=q.receiver || e.entry!=q.entry || e.quantity!=part->quantity ||
            (e.copper!=0 && e.copper!=q.cod) || e.moneyBefore<e.copper || e.moneyAfter!=e.moneyBefore-e.copper ||
            uint64_t(e.inventoryBefore)+e.quantity!=e.inventoryAfter || !e.attachmentsBefore || e.attachmentsBefore>parts.size() ||
            e.attachmentsAfter+1!=e.attachmentsBefore)return {};
        if(e.copper ? !payment.id || payment.sender!=q.receiver || payment.receiver!=q.sender || payment.money!=q.cod ||
            payment.cod || payment.attachments || payment.itemGuid || payment.itemEntry || payment.quantity || payment.subject!=subject ||
            !payment.deliveredAt || payment.expiresAt<=payment.deliveredAt : payment.id!=0)return {};
        kind="commission_customer_received";receipt=CommissionParcelReceiptId(e.sendOperation,e.item);
        guard+=" AND JSON_EXTRACT(o.after_state,'$.native.mail')="+n(e.mail)+" AND "+itemReceiptCount+'='+n(parts.size()-e.attachmentsBefore)+
            " AND NOT EXISTS(SELECT 1 FROM living_activity_operation r WHERE r.task_id=o.task_id AND r.kind='commission_parcel_returned')";
        if(q.cod)guard+=e.copper?" AND NOT EXISTS("+chargedReceipt+')':
            " AND EXISTS("+chargedReceipt+" AND JSON_EXTRACT(r.after_state,'$.payment_mail')>0)";
        nativeGuard=" AND EXISTS(SELECT 1 FROM mail m WHERE m.id="+n(e.mail)+" AND m.messageType=0 AND m.sender="+n(q.sender)+
            " AND m.receiver="+n(q.receiver)+" AND m.cod=0 AND m.money=0 AND m.subject="+SqlValue(subject)+')'+
            " AND NOT EXISTS(SELECT 1 FROM mail_items a WHERE a.mail_id="+n(e.mail)+" AND a.item_guid="+n(e.item)+')'+
            " AND (SELECT COUNT(*) FROM mail_items a WHERE a.mail_id="+n(e.mail)+")="+n(e.attachmentsAfter)+
            " AND EXISTS(SELECT 1 FROM characters c WHERE c.guid="+n(q.receiver)+" AND c.money="+n(e.moneyAfter)+')';
        if(e.copper)nativeGuard+=" AND EXISTS(SELECT 1 FROM mail m WHERE m.id="+n(payment.id)+" AND m.messageType=0"
            " AND m.sender="+n(q.receiver)+" AND m.receiver="+n(q.sender)+" AND m.money="+n(q.cod)+" AND m.cod=0"
            " AND (m.checked&8)=8 AND m.subject="+SqlValue(subject)+" AND m.deliver_time="+n(payment.deliveredAt)+
            " AND m.expire_time="+n(payment.expiresAt)+") AND NOT EXISTS(SELECT 1 FROM mail_items a WHERE a.mail_id="+n(payment.id)+')';
    }else if(e.event==CommissionMailEvent::FeeCollected) {
        if(!q.cod || e.sender!=q.receiver || e.receiver!=q.sender || e.copper!=q.cod || e.item || e.entry || e.quantity ||
            payment.id || uint64_t(e.moneyBefore)+e.copper!=e.moneyAfter || e.attachmentsBefore || e.attachmentsAfter ||
            e.inventoryBefore || e.inventoryAfter)return {};
        kind="commission_fee_collected";receipt=CommissionMailReceiptId(e.sendOperation,e.event);
        guard+=" AND EXISTS("+chargedReceipt+" AND JSON_EXTRACT(r.after_state,'$.payment_mail')="+n(e.mail)+')';
        nativeGuard=" AND EXISTS(SELECT 1 FROM mail m WHERE m.id="+n(e.mail)+" AND m.messageType=0 AND m.sender="+n(q.receiver)+
            " AND m.receiver="+n(q.sender)+" AND m.money=0 AND m.cod=0 AND (m.checked&8)=8 AND m.subject="+SqlValue(subject)+')'+
            " AND NOT EXISTS(SELECT 1 FROM mail_items a WHERE a.mail_id="+n(e.mail)+')'+
            " AND EXISTS(SELECT 1 FROM characters c WHERE c.guid="+n(q.sender)+" AND c.money="+n(e.moneyAfter)+')';
    }else {
        if(e.sender!=q.receiver || e.receiver!=q.sender || e.copper || payment.id!=e.mail || payment.sender!=e.sender ||
            payment.receiver!=e.receiver || payment.money || payment.cod || !payment.attachments || payment.attachments>parts.size() ||
            payment.subject!=subject || !payment.deliveredAt || payment.expiresAt<=payment.deliveredAt ||
            e.attachmentsBefore!=payment.attachments || e.attachmentsAfter!=payment.attachments ||
            e.item!=payment.itemGuid || e.entry!=payment.itemEntry || e.quantity!=payment.quantity ||
            e.moneyBefore || e.moneyAfter || e.inventoryBefore || e.inventoryAfter)return {};
        if(payment.attachments==1) {
            const auto part=std::find_if(parts.begin(),parts.end(),[&](const auto& p){return p.item==payment.itemGuid && p.quantity==payment.quantity;});
            if(part==parts.end() || payment.itemEntry!=q.entry)return {};
        }else if(payment.itemGuid || payment.itemEntry || payment.quantity)return {};
        kind="commission_parcel_returned";receipt=CommissionMailReceiptId(e.sendOperation,e.event);
        guard+=" AND "+itemReceiptCount+'='+n(parts.size()-payment.attachments);
        nativeGuard=" AND EXISTS(SELECT 1 FROM mail m WHERE m.id="+n(e.mail)+" AND m.messageType=0 AND m.sender="+n(q.receiver)+
            " AND m.receiver="+n(q.sender)+" AND m.money=0 AND m.cod=0 AND (m.checked&2)=2 AND m.subject="+SqlValue(subject)+
            " AND m.deliver_time="+n(payment.deliveredAt)+" AND m.expire_time="+n(payment.expiresAt)+')'+
            " AND (SELECT COUNT(*) FROM mail_items a WHERE a.mail_id="+n(e.mail)+")="+n(payment.attachments);
    }
    // Remaining attachments must be the exact originally quoted native IDs,
    // quantities and owner. A forwarded, foreign or duplicated stack cannot
    // satisfy an original parcel observation.
    if(e.event!=CommissionMailEvent::FeeCollected) {
        std::string allowed;
        for(const auto& part:parts) {
            if(!allowed.empty())allowed+=" OR ";
            allowed+="(a.item_guid="+n(part.item)+" AND i.count="+n(part.quantity)+')';
        }
        nativeGuard+=" AND NOT EXISTS(SELECT 1 FROM mail_items a LEFT JOIN item_instance i ON i.guid=a.item_guid"
            " WHERE a.mail_id="+n(e.mail)+" AND (i.guid IS NULL OR a.receiver<>"+n(e.receiver)+" OR i.owner_guid<>"+n(e.receiver)+
            " OR a.item_template<>"+n(q.entry)+" OR i.itemEntry<>"+n(q.entry)+" OR NOT ("+allowed+")))";
        nativeGuard+=" AND NOT EXISTS(SELECT 1 FROM mail_items a JOIN mail_items other ON other.item_guid=a.item_guid"
            " AND other.mail_id<>a.mail_id WHERE a.mail_id="+n(e.mail)+')'+
            " AND NOT EXISTS(SELECT 1 FROM mail_items a JOIN character_inventory v ON v.item=a.item_guid WHERE a.mail_id="+n(e.mail)+')'+
            " AND NOT EXISTS(SELECT 1 FROM mail_items a JOIN guild_bank_item g ON g.item_guid=a.item_guid WHERE a.mail_id="+n(e.mail)+')';
    }
    const auto after="{\"version\":2,\"send_operation\":\""+e.sendOperation+"\",\"mail\":"+n(e.mail)+
        ",\"sender\":"+n(e.sender)+",\"receiver\":"+n(e.receiver)+",\"item\":"+n(e.item)+",\"entry\":"+n(e.entry)+
        ",\"quantity\":"+n(e.quantity)+",\"copper\":"+n(e.copper)+",\"money_before\":"+n(e.moneyBefore)+
        ",\"money_after\":"+n(e.moneyAfter)+",\"inventory_before\":"+n(e.inventoryBefore)+",\"inventory_after\":"+n(e.inventoryAfter)+
        ",\"payment_mail\":"+n(e.event==CommissionMailEvent::CustomerReceived?payment.id:0)+
        ",\"attachments_before\":"+n(e.attachmentsBefore)+",\"attachments_after\":"+n(e.attachmentsAfter)+
        ",\"observed_at_ms\":"+n(e.atMs)+'}';
    const auto storedAfter=returning?"JSON_SET("+SqlValue(after)+",'$.returned_items',(SELECT JSON_ARRAYAGG(JSON_OBJECT('item',a.item_guid,'quantity',i.count))"
        " FROM mail_items a JOIN item_instance i ON i.guid=a.item_guid WHERE a.mail_id="+n(e.mail)+"))":SqlValue(after);
    return "INSERT INTO living_activity_operation(operation_id,task_id,task_revision,kind,request_hash,state,native_reference,"
        "before_state,after_state,evidence_code,created_at_ms,updated_at_ms) SELECT "+SqlValue(receipt)+",o.task_id,t.revision,"+
        SqlValue(kind)+",SHA2("+storedAfter+",256),'verified',"+SqlValue("mail:"+n(e.mail))+",'{}',"+storedAfter+
        ",'native_mail_transaction_observed',"+n(e.atMs)+','+n(e.atMs)+" FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
        " WHERE "+guard+nativeGuard+" ON DUPLICATE KEY UPDATE operation_id=VALUES(operation_id)";
}
}
