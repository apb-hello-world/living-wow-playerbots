#include "botpch.h"
#include "LivingNativeCommissionReceipts.h"
#include "LivingActivityCoordinator.h"
#include "Mails/Mail.h"
#include <chrono>

namespace LivingActivity {
namespace {
bool ReadVerifiedParcelQuote(const std::string& operation,CommissionMailObservation& e) {
    if(!IsUuid(operation))return false;
    auto row=CharacterDatabase.PQuery("SELECT before_state FROM living_activity_operation WHERE operation_id='%s' "
        "AND kind='commission_mail_send' AND state='verified' AND evidence_code='native_commission_parcel_and_postage_observed'",operation.c_str());
    if(!row)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(row->Fetch()[0].GetCppString());boost::property_tree::read_json(in,p);
        std::ostringstream json;boost::property_tree::write_json(json,p.get_child("native.native"),false);
        CommissionMailQuote q;if(!DecodeCommissionMailQuote(json.str(),q))return false;
        if(!q.additional.empty())e.parcelQuote=EncodeCommissionMailQuote(q);
        return true;
    }catch(const std::exception&){return false;}
}
void Record(CommissionMailObservation e) {
    if(!sLivingActivityCoordinator.OnWorldThread() || !CharacterDatabase.HasOpenTransaction())return;
    e.atMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto sql=CommissionMailObservationWrite(e);
    if(!sql.empty())CharacterDatabase.Execute(sql.c_str());
}
}
CommissionMailObservation ObserveCommissionCollectionBefore(Player& actor,const Mail& mail,const Item& item) {
    CommissionMailObservation e;
    e.sendOperation=CommissionMailOperationFromSubject(mail.subject);
    if(e.sendOperation.empty() || mail.messageType!=MAIL_NORMAL || mail.items.empty() || mail.items.size()>12 || mail.money ||
        mail.receiverGuid!=actor.GetObjectGuid() || !ReadVerifiedParcelQuote(e.sendOperation,e) ||
        (e.parcelQuote.empty() && mail.items.size()!=1) ||
        std::none_of(mail.items.begin(),mail.items.end(),[&](const auto& attached){return attached.item_guid==item.GetGUIDLow();}))return {};
    e.mail=mail.messageID;e.sender=mail.sender;e.receiver=actor.GetGUIDLow();
    e.item=item.GetGUIDLow();e.entry=item.GetEntry();e.quantity=item.GetCount();e.copper=mail.COD;
    e.moneyBefore=actor.GetMoney();e.inventoryBefore=actor.GetItemCount(e.entry,false);
    if(!e.parcelQuote.empty())e.attachmentsBefore=uint32_t(mail.items.size());return e;
}
void RecordCommissionCollection(Player& actor,CommissionMailObservation e,const NormalMailCapture& capture) {
    if(e.sendOperation.empty() || !capture.Valid() || capture.Rows().size()!=(e.copper?1u:0u))return;
    e.moneyAfter=actor.GetMoney();e.inventoryAfter=actor.GetItemCount(e.entry,false);
    if(!e.parcelQuote.empty()) {
        const auto* mail=actor.GetMail(e.mail);if(!mail)return;
        e.attachmentsAfter=uint32_t(mail->items.size());
    }
    if(e.copper)e.generated=capture.Rows().front();Record(std::move(e));
}
void RecordCommissionFeeCollection(Player& actor,const Mail& mail,uint32_t before,uint32_t amount) {
    CommissionMailObservation e;e.sendOperation=CommissionMailOperationFromSubject(mail.subject);
    if(e.sendOperation.empty() || mail.messageType!=MAIL_NORMAL || !(mail.checked&MAIL_CHECK_MASK_COD_PAYMENT) ||
        // HasItems() is the legacy template-generation flag, not the actual
        // attachment list. Pinned core leaves that flag uninitialized on new
        // online money-only mail. Inspect custody and reject templates instead.
        mail.receiverGuid!=actor.GetObjectGuid() || mail.mailTemplateId || !mail.items.empty() || mail.COD || mail.money ||
        !ReadVerifiedParcelQuote(e.sendOperation,e))return;
    e.event=CommissionMailEvent::FeeCollected;e.mail=mail.messageID;e.sender=mail.sender;e.receiver=actor.GetGUIDLow();
    e.copper=amount;e.moneyBefore=before;e.moneyAfter=actor.GetMoney();Record(std::move(e));
}
void RecordCommissionReturn(const AuctionMail& mail) {
    CommissionMailObservation e;e.sendOperation=CommissionMailOperationFromSubject(mail.subject);
    if(e.sendOperation.empty() || !ReadVerifiedParcelQuote(e.sendOperation,e))return;
    e.event=CommissionMailEvent::ParcelReturned;e.mail=mail.id;e.sender=mail.sender;e.receiver=mail.receiver;
    e.item=mail.itemGuid;e.entry=mail.itemEntry;e.quantity=mail.quantity;e.generated=mail;
    if(!e.parcelQuote.empty())e.attachmentsBefore=e.attachmentsAfter=mail.attachments;
    Record(std::move(e));
}
}
