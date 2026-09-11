#include "botpch.h"
#include "PlayerbotInventoryPressure.h"
#include "LivingActivityCoordinator.h"

#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"
#include "PlayerbotAI.h"
#include "strategy/values/ItemUsageValue.h"

using namespace ai;

PlayerbotInventoryPressure& PlayerbotInventoryPressure::instance()
{
    static PlayerbotInventoryPressure pressure;
    return pressure;
}

LivingWowItemDisposition PlayerbotInventoryPressure::Classify(Player* bot, Item* item, bool* hardReserved) const
{
    if (hardReserved)
        *hardReserved = false;
    if (!bot || !item || !bot->GetPlayerbotAI())
        return LivingWowItemDisposition::Keep;

    const auto reservations=sLivingActivityCoordinator.ResourceReservations().Inspect();
    // A mixed/uncertain stack is not a capacity candidate. Never count it as
    // disposable while waiting for an identity-safe split or claim restore.
    if (!reservations || reservations->UnreservedItem(bot->GetGUIDLow(),item->GetGUIDLow(),item->GetEntry(),item->GetCount())!=item->GetCount() ||
        sPlayerbotActionBroker.IsItemReserved(item->GetGUIDLow()) ||
        sGuildSupplies.Reserved(item->GetGUIDLow()) ||
        sGuildSupplies.ReservedEntry(bot->GetGUIDLow(),item->GetEntry()))
    {
        if (hardReserved)
            *hardReserved = true;
        return LivingWowItemDisposition::Keep;
    }

    // TBC throwing weapons are non-stackable. ItemUsage may call every bag
    // copy an equipment candidate rather than ammunition, so recognize the
    // redundant copies before consulting that mutable classification.
    ItemPrototype const* proto = item->GetProto();
    Item* equippedRanged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (proto && proto->InventoryType == INVTYPE_THROWN && proto->SellPrice && equippedRanged &&
        equippedRanged != item && equippedRanged->GetProto() &&
        equippedRanged->GetProto()->ItemId == proto->ItemId)
        return LivingWowItemDisposition::Vendor;

    ItemQualifier qualifier(item);
    ItemUsage usage = bot->GetPlayerbotAI()->GetAiObjectContext()->
        GetValue<ItemUsage>("item usage", qualifier.GetQualifier())->Get();
    switch (usage)
    {
    case ItemUsage::ITEM_USAGE_VENDOR:
    case ItemUsage::ITEM_USAGE_BAD_EQUIP:
    case ItemUsage::ITEM_USAGE_FORCE_GREED:
        return item->GetProto() && item->GetProto()->SellPrice ?
            LivingWowItemDisposition::Vendor : LivingWowItemDisposition::Keep;
    case ItemUsage::ITEM_USAGE_AH:
    case ItemUsage::ITEM_USAGE_BROKEN_AH:
        return LivingWowItemDisposition::Auction;
    case ItemUsage::ITEM_USAGE_BANK:
        return LivingWowItemDisposition::Bank;
    case ItemUsage::ITEM_USAGE_SKILL:
    case ItemUsage::ITEM_USAGE_DISENCHANT:
        return LivingWowItemDisposition::Craft;
    case ItemUsage::ITEM_USAGE_AMMO:
        return LivingWowItemDisposition::Keep;
    case ItemUsage::ITEM_USAGE_NONE:
        // Unknown is not permission to liquidate. The normal autonomous AI may
        // revisit it later after a goal or market snapshot supplies a reason.
        return LivingWowItemDisposition::Keep;
    default:
        return LivingWowItemDisposition::Keep;
    }
}

LivingWowInventoryPressureSummary PlayerbotInventoryPressure::Analyze(Player* bot) const
{
    LivingWowInventoryPressureSummary summary;
    if (!bot || !bot->GetPlayerbotAI())
        return summary;
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    summary.bagUsage = ai->GetAiObjectContext()->GetValue<uint8>("bag space")->Get();
    summary.bankUsage = ai->GetAiObjectContext()->GetValue<uint8>("bank space")->Get();
    std::list<Item*> items = ai->InventoryParseItems("inventory", IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
    for (Item* item : items)
    {
        bool reserved = false;
        LivingWowItemDisposition disposition = Classify(bot, item, &reserved);
        if (reserved)
            ++summary.reservedStacks;
        switch (disposition)
        {
        case LivingWowItemDisposition::Vendor: ++summary.vendorStacks; break;
        case LivingWowItemDisposition::Auction: ++summary.auctionStacks; break;
        case LivingWowItemDisposition::Bank: ++summary.bankStacks; break;
        case LivingWowItemDisposition::Craft: ++summary.craftStacks; break;
        default: ++summary.keepStacks; break;
        }
    }
    return summary;
}

void PlayerbotInventoryPressure::Defer(Player* bot, const LivingWowInventoryPressureSummary& summary,
    const std::string& reason) const
{
    if (!bot)
        return;
    CharacterDatabase.PExecute(
        "UPDATE organic_economy_goal SET state='expired' WHERE character_guid='%u' "
        "AND goal_type='storage_pressure' AND state IN ('candidate','proposed','active')",
        bot->GetGUIDLow());
    CharacterDatabase.PExecute(
        "INSERT INTO organic_economy_goal "
        "(character_guid,goal_type,capability_ref,state,utility,source,authoritative_payload,expires_at) "
        "VALUES ('%u','storage_pressure','storage:%u','candidate',100,'inventory_pressure',"
        "'{\"bag_usage\":%u,\"bank_usage\":%u,\"vendor_stacks\":%u,\"bank_stacks\":%u,"
        "\"auction_stacks\":%u,\"craft_stacks\":%u,\"reserved_stacks\":%u,\"reason\":\"%s\"}',"
        "DATE_ADD(NOW(),INTERVAL 2 HOUR))",
        bot->GetGUIDLow(), bot->GetGUIDLow(), (uint32)summary.bagUsage, (uint32)summary.bankUsage,
        summary.vendorStacks, summary.bankStacks, summary.auctionStacks, summary.craftStacks,
        summary.reservedStacks, reason.c_str());
    sLog.outString("Living WoW inventory pressure bot=%u result=deferred reason=%s bag=%u bank=%u vendor=%u bankable=%u auction=%u craft=%u reserved=%u",
        bot->GetGUIDLow(), reason.c_str(), (uint32)summary.bagUsage, (uint32)summary.bankUsage,
        summary.vendorStacks, summary.bankStacks, summary.auctionStacks, summary.craftStacks,
        summary.reservedStacks);
}

const char* PlayerbotInventoryPressure::Name(LivingWowItemDisposition disposition)
{
    switch (disposition)
    {
    case LivingWowItemDisposition::Vendor: return "vendor";
    case LivingWowItemDisposition::Auction: return "auction";
    case LivingWowItemDisposition::Bank: return "bank";
    case LivingWowItemDisposition::Craft: return "craft";
    default: return "keep";
    }
}
