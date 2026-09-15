#pragma once

namespace LivingActivity {
// Shared by guild and commission parcels. Only a genuinely empty, compatible
// slot is eligible; native SplitItem remains the inventory mutation authority.
inline bool EmptyNativeParcelSlot(Player& actor,Item& item,uint32_t quantity,uint16_t& result) {
    auto fits=[&](uint8_t bag,uint8_t slot) {
        const auto position=uint16_t(uint16_t(bag)<<8|slot);
        if(actor.GetItemByPos(position) || !Player::IsInventoryPos(position))return false;
        ItemPosCountVec positions;
        if(actor.CanStoreNewItem(bag,slot,positions,item.GetEntry(),quantity)!=EQUIP_ERR_OK ||
            positions.size()!=1 || positions[0].pos!=position || positions[0].count!=quantity)return false;
        result=position;return true;
    };
    for(uint8_t slot=INVENTORY_SLOT_ITEM_START;slot<INVENTORY_SLOT_ITEM_END;++slot)
        if(fits(INVENTORY_SLOT_BAG_0,slot))return true;
    for(uint8_t bag=INVENTORY_SLOT_BAG_START;bag<INVENTORY_SLOT_BAG_END;++bag) {
        auto* container=static_cast<Bag*>(actor.GetItemByPos(INVENTORY_SLOT_BAG_0,bag));
        if(container)for(uint8_t slot=0;slot<container->GetBagSize();++slot)if(fits(bag,slot))return true;
    }
    return false;
}
}
