#include "botpch.h"
#include "LivingTradeCapture.h"
#include "LivingActivityCoordinator.h"

namespace LivingActivity {
TradeItemTransfer ObserveNativeTradeTransfer(Player& sender,Player& receiver,Item& item) {
    if(!NativeTradeCapture::Active())return {};
    TradeItemTransfer result;
    result.sender=sender.GetGUIDLow();result.receiver=receiver.GetGUIDLow();
    result.item=item.GetGUIDLow();result.entry=item.GetEntry();result.quantity=item.GetCount();
    result.valid=sLivingActivityCoordinator.OnWorldThread() && item.GetOwnerGuid()==sender.GetObjectGuid();
    return result;
}
void ObserveNativeTradeDestination(Player& receiver,TradeItemTransfer& transfer,uint16_t position,uint32_t quantity) {
    if(!NativeTradeCapture::Active() || !transfer.item)return;
    if(receiver.GetGUIDLow()!=transfer.receiver || !Player::IsInventoryPos(position) ||
        transfer.destinations.size()>=256 || !quantity){transfer.valid=false;return;}
    const auto* item=receiver.GetItemByPos(position);
    if(item && (item->GetEntry()!=transfer.entry || item->GetOwnerGuid()!=receiver.GetObjectGuid())) {
        transfer.valid=false;return;
    }
    transfer.destinations.push_back({position,quantity,item?item->GetGUIDLow():0,item?item->GetCount():0,0,0});
}
void RecordNativeTradeTransfer(Player& receiver,TradeItemTransfer transfer) {
    if(!NativeTradeCapture::Active())return;
    if(receiver.GetGUIDLow()!=transfer.receiver || !sLivingActivityCoordinator.OnWorldThread())transfer.valid=false;
    for(auto& destination:transfer.destinations) {
        const auto* item=receiver.GetItemByPos(destination.position);
        if(!item || item->GetOwnerGuid()!=receiver.GetObjectGuid() || item->GetEntry()!=transfer.entry) {
            transfer.valid=false;continue;
        }
        destination.afterItem=item->GetGUIDLow();destination.afterCount=item->GetCount();
    }
    NativeTradeCapture::Observe(std::move(transfer));
}
void RecordNativeTradeCompletion(Player& first,Player& second) {
    if(!NativeTradeCapture::Active())return;
    // This hook belongs after BOTH native inventory/money saves, inside the
    // same transaction as the effect. The managed caller still awaits its
    // durable journal acknowledgement before it can complete an obligation.
    if(!sLivingActivityCoordinator.OnWorldThread() || !CharacterDatabase.HasOpenTransaction()) {
        NativeTradeCapture::Complete({});return;
    }
    NativeTradeCapture::Complete({first.GetGUIDLow(),second.GetGUIDLow(),first.GetMoney(),second.GetMoney()});
}
}
