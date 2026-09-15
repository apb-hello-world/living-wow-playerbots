#pragma once
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

class Player;
class Item;

namespace LivingActivity {
// Synchronous native evidence, not trade permission or task completion. Every
// destination is captured around the real MoveItemToInventory call, including
// stack merges that destroy the original source Item object.
struct TradeDestination {
    uint16_t position=0;
    uint32_t quantity=0,beforeItem=0,beforeCount=0,afterItem=0,afterCount=0;
};
struct TradeItemTransfer {
    uint32_t sender=0,receiver=0,item=0,entry=0,quantity=0;
    std::vector<TradeDestination> destinations;
    bool valid=true;
};
inline bool VerifiedTradeTransfer(const TradeItemTransfer& transfer) {
    if(!transfer.valid || !transfer.sender || !transfer.receiver || transfer.sender==transfer.receiver ||
        !transfer.item || !transfer.entry || !transfer.quantity || transfer.destinations.empty() ||
        transfer.destinations.size()>256)return false;
    uint64_t total=0;std::set<uint16_t> positions;std::set<uint32_t> afterItems;
    for(const auto& d:transfer.destinations) {
        if(!d.quantity || !d.afterItem || !positions.insert(d.position).second || !afterItems.insert(d.afterItem).second ||
            (d.beforeItem==0)!=(d.beforeCount==0) || d.beforeItem==transfer.item ||
            (d.beforeItem && d.afterItem!=d.beforeItem) ||
            uint64_t(d.beforeCount)+d.quantity!=d.afterCount)return false;
        total+=d.quantity;
    }
    return total==transfer.quantity;
}
struct TradeCompletion {
    uint32_t first=0,second=0,firstMoney=0,secondMoney=0;
};
class NativeTradeCapture {
public:
    NativeTradeCapture():previous(current) {
        current=this;
        if(previous){invalid=true;previous->invalid=true;}
    }
    ~NativeTradeCapture(){current=previous;}
    NativeTradeCapture(const NativeTradeCapture&)=delete;
    NativeTradeCapture& operator=(const NativeTradeCapture&)=delete;
    static bool Active(){return current!=nullptr;}
    static void Observe(TradeItemTransfer transfer) {
        if(!current)return;
        if(current->completed || current->rows.size()>=12 || !VerifiedTradeTransfer(transfer)) {
            current->invalid=true;return;
        }
        current->rows.push_back(std::move(transfer));
    }
    static void Complete(TradeCompletion result) {
        if(!current)return;
        if(current->completed || !result.first || !result.second || result.first==result.second) {
            current->invalid=true;return;
        }
        current->completed=true;current->completion=result;
    }
    bool Valid() const{return !invalid;}
    bool Completed() const{return !invalid && completed;}
    const std::vector<TradeItemTransfer>& Rows() const{return rows;}
    const TradeCompletion& Completion() const{return completion;}
private:
    inline static thread_local NativeTradeCapture* current=nullptr;
    NativeTradeCapture* previous;
    bool invalid=false,completed=false;
    std::vector<TradeItemTransfer> rows;
    TradeCompletion completion;
};
TradeItemTransfer ObserveNativeTradeTransfer(Player& sender,Player& receiver,Item& item);
void ObserveNativeTradeDestination(Player& receiver,TradeItemTransfer&,uint16_t position,uint32_t quantity);
void RecordNativeTradeTransfer(Player& receiver,TradeItemTransfer);
void RecordNativeTradeCompletion(Player& first,Player& second);
}
