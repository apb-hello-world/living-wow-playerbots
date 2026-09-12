
#include "playerbot/playerbot.h"
#include "playerbot/LivingActivityCoordinator.h"
#include "playerbot/PlayerbotServiceTracking.h"
#include "BankAction.h"
#include "playerbot/strategy/values/ItemCountValue.h"
#include "playerbot/strategy/values/ItemUsageValue.h"
#include "playerbot/PlayerbotInventoryPressure.h"
#include "playerbot/PlayerbotOrganicEconomy.h"
#include "playerbot/PlayerbotActionBroker.h"
#include "playerbot/PlayerbotGuildSupplies.h"

using namespace ai;

namespace
{
    bool UnreservedBankStack(Player* actor,const Item* item)
    {
        if (!actor || !item) return false;
        const auto held=sLivingActivityCoordinator.ResourceReservations().Inspect();
        // An unjournalled legacy transfer may merge away the source GUID.
        // Accepted stock moves only through the identity-aware service adapter,
        // including when activity execution has been paused or disabled.
        return held && held->UnreservedItem(actor->GetGUIDLow(),item->GetGUIDLow(),
            item->GetEntry(),item->GetCount())==item->GetCount();
    }
    void ResetBankActionItemCaches(PlayerbotAI* ai, const std::string& itemId, const std::string& itemQualifier,
        const std::string& usageQualifier = "")
    {
        if (!ai || itemId.empty() || itemQualifier.empty())
            return;

        AiObjectContext* context = ai->GetAiObjectContext();
        if (!context)
            return;

        // A move changes stock, usage classifications and pointer lists under
        // many qualifiers (item links, IDs, ammo, food, usage BANK, etc.).
        // Reset existing values without deleting values held by callers.
        for (const std::string& name : context->GetValues())
        {
            const std::string base = name.substr(0, name.find("::"));
            if (base == "bag space" || base == "bank space" || base == "item usage" ||
                base == "item count" || base == "bank item count" ||
                base == "inventory items" || base == "inventory item ids" || base == "bank items" ||
                base == "should bank deposit" || base == "should bank withdraw")
                if (UntypedValue* value = context->GetUntypedValue(name)) value->Reset();
        }
    }
}

bool BankAction::WithdrawForRecipe(uint32 entry, uint32 targetCount, std::string& blocker)
{
    if (!sLivingActivityCoordinator.PermitEffects(*ai, GetActivityEffects(), "native service mutation")) { blocker = "activity_authority_wait"; return false; }
    blocker = "recipe_banker_out_of_range";
    if (!bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->IsTaxiFlying() ||
        bot->GetTransport() || bot->IsBeingTeleported()) return false;
    Creature* banker = nullptr;
    for (const auto& guid : AI_VALUE(std::list<ObjectGuid>, "nearest npcs no los"))
        if ((banker = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_BANKER))) break;
    if (!banker) return false; // Never withdraw bank contents remotely.
    const uint32 before = bot->GetItemCount(entry, false), total = bot->GetItemCount(entry, true);
    if (targetCount <= before) { blocker = "recipe_materials_ready"; return false; }
    blocker = "recipe_material_reserved";
    if (sGuildSupplies.ReservedEntry(bot->GetGUIDLow(), entry) ||
        ItemUsageValue::IsNeededForQuest(bot, entry, true)) return false;
    Item* item = FindItemInBank(entry);
    if (!item) { blocker = "recipe_bank_material_missing"; return false; }
    if (sPlayerbotActionBroker.IsItemReserved(item->GetGUIDLow())) return false;
    if (!UnreservedBankStack(bot,item)) return false;
    uint32 count = std::min(targetCount - before, item->GetCount());
    ItemPosCountVec dest;
    blocker = "recipe_bag_space_unavailable";
    if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, count) != EQUIP_ERR_OK || dest.empty()) return false;
    // A partial native split targets one actual bag position and is rechecked
    // by the core. It cannot split a whole stack or consume promised stock.
    count = std::min<uint32>(count, dest.front().count);
    if (!count) return false;
    if (count < item->GetCount()) bot->SplitItem(item->GetPos(), dest.front().pos, count);
    else
    {
        uint8 bagSlot = 0;
        dest.clear();
        if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, bagSlot, false) != EQUIP_ERR_OK) return false;
        bot->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
        bot->StoreItem(dest, item, true);
    }
    ResetBankActionItemCaches(ai, std::to_string(entry), std::to_string(entry));
    const uint32 after = bot->GetItemCount(entry, false);
    const bool verified = after == before + count && bot->GetItemCount(entry, true) == total;
    blocker = verified ? "withdrew_owned_recipe_materials" : "recipe_bank_transfer_not_verified";
    PlayerbotServiceTracking::Result(bot, "profession_bank_withdraw", banker->GetEntry(), entry,
        "bag_item_count", before, verified ? after : before, true, verified ? "" : blocker.c_str());
    return verified;
}

bool BankAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    std::string text = event.getParam();

    std::list<ObjectGuid> npcs = AI_VALUE(std::list<ObjectGuid>, "nearest npcs no los");
    for (std::list<ObjectGuid>::iterator i = npcs.begin(); i != npcs.end(); i++)
    {
        Unit* npc = ai->GetUnit(*i);
        if (!npc || !npc->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_BANKER))
            continue;

        bool result = ExecuteCommand(requester, text, npc);
        return result;
    }

    if (event.getSource() != "rpg action")
        ai->TellError(requester, "Cannot find banker nearby");
    return false;
}

bool BankAction::ExecuteCommand(Player* requester, const std::string& text, Unit* bank)
{
    if (text.empty() || text == "?")
    {
        ListItems(requester);
        return true;
    }

    bool result = false;
    if (text[0] == '-')
    {
        std::list<Item*> found = ai->InventoryParseItems(text.substr(1), IterateItemsMask::ITERATE_ITEMS_IN_BANK);
        for (std::list<Item*>::iterator i = found.begin(); i != found.end(); i++)
        {
            Item* item = *i;
            result |= Withdraw(requester, item->GetProto()->ItemId);
        }
    }
    else
    {
        bool safeStorage = text == "living-wow-safe-storage";
        std::list<Item*> found = ai->InventoryParseItems(
            safeStorage ? "inventory" : text, IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
        if (found.empty())
            return false;

        for (std::list<Item*>::iterator i = found.begin(); i != found.end(); i++)
        {
            Item* item = *i;
            if (!item)
                continue;

            if (safeStorage)
            {
                LivingWowItemDisposition disposition = sPlayerbotInventoryPressure.Classify(bot, item);
                if (disposition != LivingWowItemDisposition::Bank &&
                    disposition != LivingWowItemDisposition::Craft &&
                    disposition != LivingWowItemDisposition::Auction)
                    continue;
            }

            // The validated maintenance path already reports one useful
            // summary to party chat. Keep its internal item transfers quiet;
            // explicit player-issued bank commands retain normal feedback.
            result |= Deposit(safeStorage ? nullptr : requester, item);
        }
    }

    return result;
}

bool BankAction::Withdraw(Player* requester, const uint32 itemid)
{
    if (!sLivingActivityCoordinator.PermitEffects(*ai, GetActivityEffects(), "native service mutation")) return false;
    Item* pItem = FindItemInBank(itemid);
    if (!pItem || !UnreservedBankStack(bot,pItem))
        return false;

    const ItemPrototype* proto = pItem->GetProto();
    if (!proto)
        return false;

    const std::string itemId = std::to_string(proto->ItemId);
    const std::string itemQualifier = ItemQualifier(pItem).GetQualifier();
    const std::string itemText = chat->formatItem(pItem, pItem->GetCount());

    ResetBankActionItemCaches(ai, itemId, itemQualifier);

    ItemPosCountVec dest;
    uint8 bagSlot;
    InventoryResult msg = bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, pItem, bagSlot, false);

    if (msg != EQUIP_ERR_OK)
    {
        PlayerbotServiceTracking::Result(bot, "bank_withdraw", 0, itemid, "bag_item_count", 0, 0,
            true, ("inventory_error_" + std::to_string(unsigned(msg))).c_str());
        bot->SendEquipError(msg, pItem, NULL);
        return false;
    }

    const uint32 beforeCount = bot->GetItemCount(itemid, false);
    bot->RemoveItem(pItem->GetBagSlot(), pItem->GetSlot(), true);
    bot->StoreItem(dest, pItem, true);
    const bool verified = PlayerbotServiceTracking::Result(bot, "bank_withdraw", 0, itemid,
        "bag_item_count", beforeCount, bot->GetItemCount(itemid, false));
    ResetBankActionItemCaches(ai, itemId, itemQualifier);

    std::ostringstream out;
    out << "got " << itemText << " from bank";
    if (requester)
        ai->TellPlayer(requester, out.str());
    return verified;
}

bool BankAction::Deposit(Player* requester, Item* pItem)
{
    if (!sLivingActivityCoordinator.PermitEffects(*ai, GetActivityEffects(), "native service mutation")) return false;
    std::ostringstream out;

    const ItemPrototype* proto = pItem ? pItem->GetProto() : nullptr;
    if (!proto)
        return false;
    if (!UnreservedBankStack(bot,pItem)) return false;
    if (sPlayerbotOrganicEconomy.RecipeMaterialQuantity(bot->GetGUIDLow(), pItem->GetEntry())) return false;

    const std::string itemId = std::to_string(proto->ItemId);
    const std::string itemQualifier = ItemQualifier(pItem).GetQualifier();
    const std::string itemText = chat->formatItem(pItem, pItem->GetCount());

    ResetBankActionItemCaches(ai, itemId, itemQualifier);

    ItemPosCountVec dest;
    uint8 bagSlot;
    InventoryResult msg = bot->CanBankItem(NULL_BAG, NULL_SLOT, dest, pItem, false, bagSlot);

    if (msg != EQUIP_ERR_OK)
    {
        PlayerbotServiceTracking::Result(bot, "bank_deposit", 0, proto->ItemId, "bank_item_count", 0, 0,
            true, ("inventory_error_" + std::to_string(unsigned(msg))).c_str());
        bot->SendEquipError(msg, pItem, NULL);
        return false;
    }

    const uint32 beforeCount = bot->GetItemCount(proto->ItemId, true) - bot->GetItemCount(proto->ItemId, false);
    bot->RemoveItem(pItem->GetBagSlot(), pItem->GetSlot(), true);
    bot->BankItem(dest, pItem, true);
    const bool verified = PlayerbotServiceTracking::Result(bot, "bank_deposit", 0, proto->ItemId, "bank_item_count",
        beforeCount, bot->GetItemCount(proto->ItemId, true) - bot->GetItemCount(proto->ItemId, false));
    ResetBankActionItemCaches(ai, itemId, itemQualifier);

    out << "put " << itemText << " to bank";
    if (requester)
        ai->TellPlayer(requester, out.str());
	return verified;
}

void BankAction::ListItems(Player* requester)
{
    ai->TellPlayer(requester, "=== Bank ===");

    std::map<uint32, int> items;
    std::map<uint32, bool> soulbound;
    for (int i = BANK_SLOT_ITEM_START; i < BANK_SLOT_ITEM_END; ++i)
    {
        if (Item* pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pItem)
            {
                items[pItem->GetProto()->ItemId] += pItem->GetCount();
                soulbound[pItem->GetProto()->ItemId] = pItem->IsSoulBound();
            }
        }
    }

    for (int i = BANK_SLOT_BAG_START; i < BANK_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pBag)
            {
                for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
                {
                    if (Item* pItem = pBag->GetItemByPos(j))
                    {
                        if (pItem)
                        {
                            items[pItem->GetProto()->ItemId] += pItem->GetCount();
                            soulbound[pItem->GetProto()->ItemId] = pItem->IsSoulBound();
                        }
                    }
                }
            }
        }
    }

    ai->InventoryTellItems(requester, items, soulbound);
}

Item* BankAction::FindItemInBank(uint32 ItemId)
{
    for (uint8 slot = BANK_SLOT_ITEM_START; slot < BANK_SLOT_ITEM_END; slot++)
    {
        Item* const pItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (pItem)
        {
            const ItemPrototype* const pItemProto = pItem->GetProto();
            if (!pItemProto)
                continue;

            if (pItemProto->ItemId == ItemId)   // have required item
                return pItem;
        }
    }

    for (uint8 bag = BANK_SLOT_BAG_START; bag < BANK_SLOT_BAG_END; ++bag)
    {
        const Bag* const pBag = (Bag *) bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
        if (pBag)
        {
            for (uint8 slot = 0; slot < pBag->GetBagSize(); ++slot)
            {
                Item* const pItem = bot->GetItemByPos(bag, slot);
                if (pItem)
                {
                    const ItemPrototype* const pItemProto = pItem->GetProto();
                    if (!pItemProto)
                        continue;

                    if (pItemProto->ItemId == ItemId)
                        return pItem;
                }
            }
        }
    }

    return NULL;
}

bool BankAction::AutoDeposit()
{
    if (!sLivingActivityCoordinator.PermitEffects(*ai, GetActivityEffects(), "native service mutation")) return false;
    bool deposited = false;
    ResetBankActionItemCaches(ai, "all", "all");

    std::string itemusageQualifier = "usage " + std::to_string((uint8)ItemUsage::ITEM_USAGE_BANK);

    std::vector<ObjectGuid> items;
    for (Item* item : ai->InventoryParseItems("all", IterateItemsMask::ITERATE_ITEMS_IN_BAGS))
        if (item) items.push_back(item->GetObjectGuid());
    for (const ObjectGuid& guid : items)
    {
        Item* item = bot->GetItemByGuid(guid);
        if (!item || !UnreservedBankStack(bot,item))
            continue;

        const ItemPrototype* proto = item->GetProto();
        if (!proto)
            continue;

        const std::string itemId = std::to_string(proto->ItemId);
        const std::string itemQualifier = ItemQualifier(item).GetQualifier();

        // Re-evaluate after every transfer; the original list is only a snapshot.
        ItemQualifier qualifier(item);
        std::string qualStr = qualifier.GetQualifier();
        ItemUsage currentUsage = AI_VALUE2(ItemUsage, "item usage", qualStr);
        if (currentUsage != ItemUsage::ITEM_USAGE_BANK)
            continue;
        if (sPlayerbotOrganicEconomy.RecipeMaterialQuantity(bot->GetGUIDLow(), item->GetEntry())) continue;

        ItemPosCountVec dest;
        uint8 bagSlot;
        InventoryResult msg = bot->CanBankItem(NULL_BAG, NULL_SLOT, dest, item, false, bagSlot);

        if (msg != EQUIP_ERR_OK)
        {
            PlayerbotServiceTracking::Result(bot, "bank_deposit", 0, proto->ItemId, "bank_item_count", 0, 0,
                true, ("inventory_error_" + std::to_string(unsigned(msg))).c_str());
            continue;
        }

        const uint32 beforeCount = bot->GetItemCount(proto->ItemId, true) - bot->GetItemCount(proto->ItemId, false);
        bot->RemoveItem(item->GetBagSlot(), item->GetSlot(), true);
        bot->BankItem(dest, item, true);
        const bool verified = PlayerbotServiceTracking::Result(bot, "bank_deposit", 0, proto->ItemId, "bank_item_count",
            beforeCount, bot->GetItemCount(proto->ItemId, true) - bot->GetItemCount(proto->ItemId, false));
        ResetBankActionItemCaches(ai, itemId, itemQualifier, itemusageQualifier);
        deposited |= verified;
    }

    return deposited;
}

bool BankAction::AutoWithdraw()
{
    if (!sLivingActivityCoordinator.PermitEffects(*ai, GetActivityEffects(), "native service mutation")) return false;
    bool withdrew = false;
    ResetBankActionItemCaches(ai, "all", "all");

    if (AI_VALUE(uint8, "bag space") > 80)
        return false;

    auto checkAndWithdraw = [&](Item* pItem) -> bool
    {
        if (!pItem || !UnreservedBankStack(bot,pItem))
            return false;

        ItemPrototype const* proto = pItem->GetProto();
        if (!proto)
            return false;
        // The exact recipe executor withdraws only the missing quantity.
        if (sPlayerbotOrganicEconomy.RecipeMaterialQuantity(bot->GetGUIDLow(), pItem->GetEntry())) return false;

        const std::string itemId = std::to_string(proto->ItemId);
        const std::string itemQualifier = ItemQualifier(pItem).GetQualifier();

        if (AI_VALUE(uint8, "bag space") > 80) return false;
        ItemUsage usage = ItemUsageValue::ForBankWithdrawal(ai, pItem);
        if (usage == ItemUsage::ITEM_USAGE_BANK)
            return false;        

        ItemPosCountVec dest;
        uint8 bagSlot;
        InventoryResult msg = bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, pItem, bagSlot, false);

        if (msg != EQUIP_ERR_OK)
        {
            PlayerbotServiceTracking::Result(bot, "bank_withdraw", 0, proto->ItemId, "bag_item_count", 0, 0,
                true, ("inventory_error_" + std::to_string(unsigned(msg))).c_str());
            return false;
        }

        const uint32 beforeCount = bot->GetItemCount(proto->ItemId, false);
        bot->RemoveItem(pItem->GetBagSlot(), pItem->GetSlot(), true);
        bot->StoreItem(dest, pItem, true);
        const bool verified = PlayerbotServiceTracking::Result(bot, "bank_withdraw", 0, proto->ItemId, "bag_item_count",
            beforeCount, bot->GetItemCount(proto->ItemId, false));
        ResetBankActionItemCaches(ai, itemId, itemQualifier);
        return verified;
    };

    std::vector<ObjectGuid> items;
    for (Item* item : ai->InventoryParseItems("all", IterateItemsMask::ITERATE_ITEMS_IN_BANK))
        if (item) items.push_back(item->GetObjectGuid());
    for (const ObjectGuid& guid : items)
    {
        if (checkAndWithdraw(bot->GetItemByGuid(guid)))
            withdrew = true;
    }    

    return withdrew;
}
