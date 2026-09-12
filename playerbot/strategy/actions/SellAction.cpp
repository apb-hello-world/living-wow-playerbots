
#include "playerbot/playerbot.h"
#include "playerbot/LivingActivityCoordinator.h"
#include "playerbot/PlayerbotServiceTracking.h"
#include "SellAction.h"
#include "playerbot/PlayerbotGuildSupplies.h"
#include "playerbot/strategy/ItemVisitors.h"
#include "playerbot/strategy/values/ItemUsageValue.h"
#include "playerbot/PlayerbotInventoryPressure.h"

using namespace ai;

class SellItemsVisitor : public IterateItemsVisitor
{
public:
    SellItemsVisitor(SellAction* action) : IterateItemsVisitor()
    {
        this->action = action;
    }

    virtual bool Visit(Item* item)
    {
        action->Sell(nullptr, item);
        return true;
    }

private:
    SellAction* action;
};

bool SellAction::Execute(Event& event)
{
    static uint32 minAutoSellItems = 5;
    static uint8 minAutoSellPercentageOfBag = 20;
    static uint8 maxAutoSellPercentageOfBag = 80;


    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();

    std::string text = event.getParam();

    if (text == "*" || text.empty())
        text = "gray";

    // LIV-71 party maintenance must execute the same authoritative
    // disposition used during preflight. Re-parsing only "usage 12" excluded
    // safe BAD_EQUIP/FORCE_GREED stacks that preflight had already approved.
    std::list<Item*> items = ai->InventoryParseItems(
        event.getSource() == "rpg action" && text == "living-wow-safe-vendor" ? "inventory" : text,
        IterateItemsMask::ITERATE_ITEMS_IN_BAGS);

    if (event.getSource() == "rpg action")
    {
        items.sort([](Item* i, Item* j) {return i->GetProto()->SellPrice * i->GetCount() < j->GetProto()->SellPrice * j->GetCount(); }); //Sell cheapest items first.
    }

    uint32 soldItems = 0;
    uint32 shouldSell = std::max(minAutoSellItems, uint32(items.size() * urand(minAutoSellPercentageOfBag, maxAutoSellPercentageOfBag) / 100));
    for (std::list<Item*>::iterator i = items.begin(); i != items.end(); ++i)
    {
        if (event.getSource() == "rpg action" &&
            sPlayerbotInventoryPressure.Classify(bot, *i) != LivingWowItemDisposition::Vendor)
            continue;
        if (Sell(requester, *i))
            soldItems++;

        if (event.getSource() == "rpg action" && soldItems >= shouldSell)
            break;
    }

    return soldItems;
}

bool SellAction::Sell(Player* requester, FindItemVisitor* visitor)
{
    bool didSell = false;
    ai->InventoryIterateItems(visitor, IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
    std::list<Item*> items = visitor->GetResult();
    for (std::list<Item*>::iterator i = items.begin(); i != items.end(); ++i)
    {
        didSell |= Sell(requester, *i);
    }

    return didSell;
}

bool SellAction::Sell(Player* requester, Item* item)
{
    if (!sLivingActivityCoordinator.PermitEffects(*ai, GetActivityEffects(), "native service mutation")) return false;
    if (!item || sGuildSupplies.Reserved(item->GetGUIDLow()))
        return false;
    const auto claims=sLivingActivityCoordinator.ResourceReservations().Inspect();
    if(!claims || claims->UnreservedItem(bot->GetGUIDLow(),item->GetGUIDLow(),item->GetEntry(),item->GetCount())!=item->GetCount())
        return false; // A managed capacity sale executes through its exact native adapter, not this legacy path.
    bool didSell = false;

    std::ostringstream out;
    std::list<ObjectGuid> vendors = ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid> >("nearest npcs")->Get();

    for (std::list<ObjectGuid>::iterator i = vendors.begin(); i != vendors.end(); ++i)
    {
        ObjectGuid vendorguid = *i;
        Creature *pCreature = bot->GetNPCIfCanInteractWith(vendorguid,UNIT_NPC_FLAG_VENDOR);
        if (!pCreature)
            continue;     

        if (!item->GetProto()->SellPrice)
        {
            if(ai->HasActivePlayerMaster())
                out << "Unable to sell " << chat->formatItem(item);

            continue;
        }

        ObjectGuid itemguid = item->GetObjectGuid();
        uint32 count = item->GetCount();

        uint32 botMoney = bot->GetMoney();

        const uint32 itemEntry = item->GetEntry();
        const std::string itemText = chat->formatItem(item);
        const uint32 beforeCount = bot->GetItemCount(itemEntry, false);

        const bool itemLootOpen = bot->GetLootGuid() == itemguid;
        WorldPacket p;
        p << vendorguid << itemguid << count;
        bot->GetSession()->HandleSellItemOpcode(p);

        if (ai->HasCheat(BotCheatMask::gold))
        {
            bot->SetMoney(botMoney);
        }

        if (!PlayerbotServiceTracking::Result(bot, "sell", pCreature->GetEntry(), itemEntry, "bag_item_count",
            beforeCount, bot->GetItemCount(itemEntry, false), false,
            itemLootOpen ? "item_loot_open" : "vendor_rejected_sale"))
            return false;
        sPlayerbotAIConfig.logEvent(ai, "SellAction", itemText, std::to_string(itemEntry));
        out << "Selling " << itemText;
        if (sPlayerbotAIConfig.globalSoundEffects)
            bot->PlayDistanceSound(120);

        didSell = true;

        ai->TellPlayer(requester, out, PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
        break;
    }

    return didSell;
}
