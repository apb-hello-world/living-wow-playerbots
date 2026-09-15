#include "botpch.h"
#include "LivingNativeCommissionReceipts.h"
#include "LivingActivityCoordinator.h"
#include "Mails/Mail.h"
#include <chrono>

namespace LivingActivity {
namespace {
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
    if(e.sendOperation.empty() || mail.messageType!=MAIL_NORMAL || mail.items.size()!=1 || mail.money ||
        mail.receiverGuid!=actor.GetObjectGuid() || mail.items.front().item_guid!=item.GetGUIDLow())return {};
    e.mail=mail.messageID;e.sender=mail.sender;e.receiver=actor.GetGUIDLow();
    e.item=item.GetGUIDLow();e.entry=item.GetEntry();e.quantity=item.GetCount();e.copper=mail.COD;
    e.moneyBefore=actor.GetMoney();e.inventoryBefore=actor.GetItemCount(e.entry,false);return e;
}
void RecordCommissionCollection(Player& actor,CommissionMailObservation e,const NormalMailCapture& capture) {
    if(e.sendOperation.empty() || !capture.Valid() || capture.Rows().size()!=(e.copper?1u:0u))return;
    e.moneyAfter=actor.GetMoney();e.inventoryAfter=actor.GetItemCount(e.entry,false);
    if(e.copper)e.generated=capture.Rows().front();Record(std::move(e));
}
void RecordCommissionFeeCollection(Player& actor,const Mail& mail,uint32_t before,uint32_t amount) {
    CommissionMailObservation e;e.sendOperation=CommissionMailOperationFromSubject(mail.subject);
    if(e.sendOperation.empty() || mail.messageType!=MAIL_NORMAL || !(mail.checked&MAIL_CHECK_MASK_COD_PAYMENT) ||
        // HasItems() is the legacy template-generation flag, not the actual
        // attachment list. Pinned core leaves that flag uninitialized on new
        // online money-only mail. Inspect custody and reject templates instead.
        mail.receiverGuid!=actor.GetObjectGuid() || mail.mailTemplateId || !mail.items.empty() || mail.COD || mail.money)return;
    e.event=CommissionMailEvent::FeeCollected;e.mail=mail.messageID;e.sender=mail.sender;e.receiver=actor.GetGUIDLow();
    e.copper=amount;e.moneyBefore=before;e.moneyAfter=actor.GetMoney();Record(std::move(e));
}
void RecordCommissionReturn(const AuctionMail& mail) {
    CommissionMailObservation e;e.sendOperation=CommissionMailOperationFromSubject(mail.subject);
    if(e.sendOperation.empty())return;
    e.event=CommissionMailEvent::ParcelReturned;e.mail=mail.id;e.sender=mail.sender;e.receiver=mail.receiver;
    e.item=mail.itemGuid;e.entry=mail.itemEntry;e.quantity=mail.quantity;e.generated=mail;Record(std::move(e));
}
}
