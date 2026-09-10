#include "botpch.h"
#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"

#include "PlayerbotAI.h"
#include "PlayerbotChatDirector.h"
#include "PlayerbotChatJson.h"
#include "PlayerbotLLMInterface.h"
#include "PlayerbotRendezvousManager.h"
#include "RandomPlayerbotMgr.h"
#include "ServerFacade.h"
#include "TravelMgr.h"
#include "strategy/ItemVisitors.h"
#include "strategy/actions/MailAction.h"
#include "strategy/values/BudgetValues.h"
#include "strategy/values/CraftValues.h"
#include "strategy/values/ItemUsageValue.h"

#include <future>
#include <regex>
#include <sstream>
#include <thread>

using namespace ai;

static uint32 CountBrokerPlayerItem(Player* player, uint32 entry);

static uint32 CountBrokerBotItem(Player* bot, uint32 entry)
{
    if (!bot || !entry) return 0;
    FindItemByIdVisitor visitor(entry);
    bot->GetPlayerbotAI()->InventoryIterateItems(&visitor, IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
    uint32 count = 0;
    for (Item* item : visitor.GetResult())
        if (item) count += item->GetCount();
    return count;
}

static std::string MeetingPlayerLocation(Player* player)
{
    if (!player)
        return "your location";
    uint32 areaId = sServerFacade.GetAreaId(player);
    AreaTableEntry const* area = GetAreaEntryByAreaID(areaId);
    AreaTableEntry const* zone = GetAreaEntryByAreaID(player->GetZoneId());
    return area && area->area_name[0] ? area->area_name[0] :
        (zone && zone->area_name[0] ? zone->area_name[0] : "your location");
}

static Item* SplitBrokerItem(Player* bot, Item* item, uint32 quantity)
{
    if (!bot || !item || !quantity || item->GetCount() < quantity)
        return nullptr;
    if (item->GetCount() == quantity)
        return item;

    uint8 destinationBag = INVENTORY_SLOT_BAG_0;
    uint8 destinationSlot = 0;
    bool found = false;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END && !found; ++slot)
    {
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            destinationSlot = slot;
            found = true;
        }
    }
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END && !found; ++bagSlot)
    {
        Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot);
        if (!bag)
            continue;
        for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
        {
            if (!bag->GetItemByPos(slot))
            {
                destinationBag = bagSlot;
                destinationSlot = (uint8)slot;
                found = true;
                break;
            }
        }
    }
    if (!found)
        return nullptr;

    uint16 source = ((uint16)item->GetBagSlot() << 8) | item->GetSlot();
    uint16 destination = ((uint16)destinationBag << 8) | destinationSlot;
    uint32 entry = item->GetEntry();
    bot->SplitItem(source, destination, quantity);
    Item* split = bot->GetItemByPos(destinationBag, destinationSlot);
    return split && split->GetEntry() == entry && split->GetCount() == quantity ? split : nullptr;
}

PlayerbotActionBroker& PlayerbotActionBroker::instance()
{
    static PlayerbotActionBroker broker;
    return broker;
}

static Item* FindBrokerItem(Player* bot, uint32 itemEntry, uint32 itemGuid)
{
    FindItemByIdVisitor visitor(itemEntry);
    bot->GetPlayerbotAI()->InventoryIterateItems(&visitor, IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
    for (Item* item : visitor.GetResult())
        if (item->GetGUIDLow() == itemGuid)
            return item;
    return nullptr;
}

PlayerbotActionBroker::Transaction* PlayerbotActionBroker::Find(uint32 botGuid, uint32 playerGuid)
{
    for (auto& pair : transactions)
        if (pair.second.botGuid == botGuid && pair.second.playerGuid == playerGuid &&
            pair.second.state != "completed" && pair.second.state != "cancelled" && pair.second.state != "expired" && pair.second.state != "failed")
            return &pair.second;
    return nullptr;
}

const PlayerbotActionBroker::Transaction* PlayerbotActionBroker::Find(uint32 botGuid, uint32 playerGuid) const
{
    for (const auto& pair : transactions)
        if (pair.second.botGuid == botGuid && pair.second.playerGuid == playerGuid &&
            pair.second.state != "completed" && pair.second.state != "cancelled" && pair.second.state != "expired" && pair.second.state != "failed")
            return &pair.second;
    return nullptr;
}

void PlayerbotActionBroker::UpsertEconomicQuote(const LivingWowChatJson::EconomicQuote& value, bool update)
{
    auto found = economicQuotes.find(value.quoteId);
    if (update && found == economicQuotes.end()) return;
    EconomicQuoteState quote = found == economicQuotes.end() ? EconomicQuoteState() : found->second;
    if (found != economicQuotes.end() && (quote.botGuid != value.botGuid || quote.playerGuid != value.targetGuid ||
        quote.itemEntry != value.itemId || quote.quantity != value.quantity || quote.direction != value.direction ||
        quote.capabilityRef != value.capabilityRef)) return;
    if (found != economicQuotes.end())
    {
        bool monotonic = quote.direction == "bot_to_player" ?
            value.currentPriceCopper <= quote.currentPrice && value.currentPriceCopper >= quote.limitPrice :
            value.currentPriceCopper >= quote.currentPrice && value.currentPriceCopper <= quote.limitPrice;
        if (!monotonic || value.roundsUsed < quote.rounds || value.roundsUsed > quote.maximumRounds) return;
    }
    quote.quoteId = value.quoteId; quote.state = value.state; quote.botGuid = value.botGuid;
    quote.playerGuid = value.targetGuid; quote.direction = value.direction; quote.itemEntry = value.itemId;
    quote.quantity = value.quantity; quote.capabilityRef = value.capabilityRef;
    quote.openingPrice = value.openingPriceCopper; quote.currentPrice = value.currentPriceCopper;
    quote.limitPrice = value.limitPriceCopper; quote.rounds = value.roundsUsed;
    quote.maximumRounds = value.maximumRounds; quote.delivery = value.delivery;
    quote.freeGiftDisposition = value.freeGiftDisposition;
    quote.expires = std::chrono::steady_clock::now() + std::chrono::seconds(std::max<uint32>(30, std::min<uint32>(1800, value.expiresInSeconds)));
    economicQuotes[quote.quoteId] = quote;
}

void PlayerbotActionBroker::AppendEconomicQuotesJson(uint32 playerGuid,
    const std::map<uint32, ChatDirectorCandidate>& candidates, std::ostringstream& json) const
{
    bool first = true;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& pair : economicQuotes)
    {
        const EconomicQuoteState& quote = pair.second;
        if (quote.playerGuid != playerGuid || now >= quote.expires || candidates.find(quote.botGuid) == candidates.end() ||
            (quote.state != "offered" && quote.state != "countered")) continue;
        if (!first) json << ','; first = false;
        json << "{\"quote_id\":\"" << quote.quoteId << "\",\"state\":\"" << quote.state
             << "\",\"bot_guid\":" << quote.botGuid << ",\"target_guid\":" << quote.playerGuid
             << ",\"direction\":\"" << quote.direction << "\",\"item_id\":" << quote.itemEntry
             << ",\"quantity\":" << quote.quantity << ",\"capability_ref\":\"" << quote.capabilityRef
             << "\",\"opening_price_copper\":" << quote.openingPrice << ",\"current_price_copper\":" << quote.currentPrice
             << ",\"limit_price_copper\":" << quote.limitPrice << ",\"rounds_used\":" << quote.rounds
             << ",\"maximum_rounds\":" << quote.maximumRounds << ",\"delivery\":\"" << quote.delivery
             << "\",\"free_gift_disposition\":\"" << quote.freeGiftDisposition << "\"}";
    }
}

PlayerbotActionResult PlayerbotActionBroker::Create(const ChatDirectorActionProposal& proposal, const ChatDirectorEvent& event)
{
    auto reject = [](const std::string& code, const std::string& message)
    {
        return PlayerbotActionResult(false, code, message);
    };
    if ((proposal.delivery != "direct" && proposal.delivery != "meeting" && proposal.delivery != "mail") ||
        (proposal.type != "give_item" && proposal.type != "sell_item" && proposal.type != "buy_item" &&
         proposal.type != "accept_player_gift" && proposal.type != "conjure_water" && proposal.type != "craft_commission"))
        return reject("unsupported_proposal", "I can't handle that kind of transaction.");
    if (proposal.botGuid == 0 || proposal.targetGuid != event.speakerGuid || proposal.quantity == 0)
        return reject("malformed_proposal", "That trade request was incomplete.");
    if (Find(proposal.botGuid, proposal.targetGuid))
        return reject("active_transaction_conflict", "We already have another trade in progress.");

    const ChatDirectorCapability* offeredCapability = nullptr;
    auto candidate = event.candidates.find(proposal.botGuid);
    if (candidate == event.candidates.end())
        return reject("bot_not_candidate", "I'm not available for that trade now.");
    EconomicQuoteState* quote = nullptr;
    if (!proposal.quoteId.empty())
    {
        auto found = economicQuotes.find(proposal.quoteId);
        if (found == economicQuotes.end() || std::chrono::steady_clock::now() >= found->second.expires)
            return reject("quote_missing_or_expired", "That price quote has expired.");
        quote = &found->second;
        bool gift = proposal.type == "accept_player_gift" && quote->direction == "player_to_bot" && proposal.priceCopper == 0;
        bool matchingType = gift || (proposal.type == "buy_item" && quote->direction == "player_to_bot") ||
            (proposal.type == "sell_item" && quote->direction == "bot_to_player");
        if (!matchingType || quote->botGuid != proposal.botGuid || quote->playerGuid != proposal.targetGuid ||
            quote->capabilityRef != proposal.capabilityRef || quote->quantity != proposal.quantity ||
            (!gift && quote->currentPrice != proposal.priceCopper))
            return reject("quote_mismatch", "That agreement no longer matches the active quote.");
        if (gift && quote->freeGiftDisposition != "accept")
            return reject("gift_disposition_changed", "I can't accept that as a free gift.");
    }
    bool negotiatedProposal = proposal.priceCopper != 0 && proposal.proposalId.compare(0, 11, "negotiated-") == 0 &&
        (proposal.type == "sell_item" || proposal.type == "buy_item");

    for (const ChatDirectorCapability& capability : candidate->second.actionCapabilities)
    {
        if (capability.capabilityRef == proposal.capabilityRef)
        {
            offeredCapability = &capability;
            break;
        }
    }
    if (!offeredCapability && !negotiatedProposal && !quote)
        return reject("missing_or_stale_capability", "That offer is no longer available.");
    if (offeredCapability && (proposal.quantity < offeredCapability->minQuantity ||
        proposal.quantity > offeredCapability->maxQuantity))
        return reject("quantity_changed", "That quantity is no longer available.");
    if (offeredCapability)
    {
        bool giftedSaleCapability = proposal.type == "give_item" && offeredCapability->type == "sell_item" &&
            offeredCapability->giftEligible;
        bool negotiatedEconomicType = negotiatedProposal &&
            (offeredCapability->type == "give_item" || offeredCapability->type == "sell_item" || offeredCapability->type == "buy_item");
        if (proposal.type != offeredCapability->type && !giftedSaleCapability && !negotiatedEconomicType)
            return reject("capability_type_mismatch", "That offer can't perform this transaction.");
    }

    std::smatch match;
    bool conjure = proposal.type == "conjure_water";
    bool crafting = proposal.type == "craft_commission";
    bool buying = proposal.type == "buy_item" || proposal.type == "accept_player_gift";
    uint32 itemEntry = 0, itemGuid = 0, spellId = 0;
    if (buying)
    {
        if (proposal.delivery == "mail") return reject("unsupported_delivery", "I can't buy that through the mail.");
        if (!std::regex_match(proposal.capabilityRef, match, std::regex(R"(buy:([0-9]+))")))
            return reject("invalid_capability_ref", "That purchase offer is invalid.");
        itemEntry = (uint32)std::stoul(match[1].str());
    }
    else if (conjure)
    {
        if (proposal.delivery == "mail" || !std::regex_match(proposal.capabilityRef, match,
            std::regex(R"(spell:conjure_water:([0-9]+):([0-9]+))")))
            return reject("invalid_capability_ref", "That conjuring offer is invalid.");
        spellId = (uint32)std::stoul(match[1].str());
        itemEntry = (uint32)std::stoul(match[2].str());
    }
    else if (crafting)
    {
        if (!std::regex_match(proposal.capabilityRef, match,
            std::regex(R"(spell:craft:([0-9]+):([0-9]+))")))
            return reject("invalid_capability_ref", "That crafting offer is invalid.");
        spellId = (uint32)std::stoul(match[1].str());
        itemEntry = (uint32)std::stoul(match[2].str());
    }
    else
    {
        if (!std::regex_match(proposal.capabilityRef, match, std::regex(R"(item:([0-9]+):([0-9]+))")))
            return reject("invalid_capability_ref", "That item offer is invalid.");
        itemEntry = (uint32)std::stoul(match[1].str());
        itemGuid = (uint32)std::stoul(match[2].str());
        if (IsItemReserved(itemGuid)) return reject("item_reserved", "That item is already reserved.");
    }

    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(proposal.botGuid);
    Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, proposal.targetGuid));
    Item* item = !conjure && !buying && bot ? FindBrokerItem(bot, itemEntry, itemGuid) : nullptr;
    bool sameZone = bot && player && bot->GetMapId() == player->GetMapId() && bot->GetZoneId() == player->GetZoneId();
    if (!bot || !player) return reject("participant_unavailable", "One of us is no longer available.");
    if (!bot->IsAlive() || !player->IsAlive()) return reject("participant_dead", "We can't trade while one of us is dead.");
    if (bot->GetTeam() != player->GetTeam()) return reject("faction_mismatch", "We can't trade across factions.");
    if (proposal.delivery != "mail" && !sameZone) return reject("incompatible_zone", "We're no longer in the same area.");
    if (proposal.delivery == "direct" && !bot->IsWithinDistInMap(player, INTERACTION_DISTANCE))
        return reject("out_of_range", "You're too far away to trade directly.");
    if (proposal.delivery == "direct" && (bot->IsInCombat() || player->IsInCombat()))
        return reject("participant_in_combat", "We can't open a trade while one of us is fighting.");
    if (!conjure && !crafting && !buying && !item) return reject("item_missing", "I no longer have that item.");
    if (!conjure && !crafting && !buying && item->GetCount() < proposal.quantity)
        return reject("quantity_changed", "I no longer have that many.");
    if (!conjure && !crafting && !buying && (!item->CanBeTraded() || item->IsSoulBound()))
        return reject("item_protected", "That item can't be traded.");
    if (!conjure && !crafting && !buying && proposal.delivery == "mail" && item->IsConjuredConsumable())
        return reject("conjured_item_mail_restricted", "Conjured items can't be mailed.");
    if (!conjure && !crafting && !buying && proposal.delivery != "mail")
    {
        ItemPosCountVec destination;
        uint8 bagSlot = 0;
        if (player->CanStoreItem(NULL_BAG, NULL_SLOT, destination, item, bagSlot, false) != EQUIP_ERR_OK)
            return reject("player_inventory_full", "You don't have room for that item.");
    }
    if (conjure && (bot->getClass() != CLASS_MAGE || proposal.quantity > 5 ||
        !bot->GetPlayerbotAI()->CanCastSpell(spellId, bot, 0)))
        return reject("ability_unavailable", "I can't conjure that right now.");
    if (crafting)
    {
        SpellEntry const* recipe = sServerFacade.LookupSpellInfo(spellId);
        bool createsItem = false;
        if (recipe && bot->HasSpell(spellId))
            for (uint32 effect = 0; effect < MAX_EFFECT_INDEX; ++effect)
                if (recipe->Effect[effect] == SPELL_EFFECT_CREATE_ITEM && recipe->EffectItemType[effect] == itemEntry)
                    createsItem = true;
        bool canCraft = createsItem && bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<bool>(
            "can craft spell", std::to_string(spellId))->Get();
        uint32 craftCount = canCraft ? bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<uint32>(
            "has reagents for", std::to_string(spellId))->Get() : 0;
        if (proposal.quantity != 1 || !craftCount)
            return reject("craft_resources_changed", "I no longer have everything needed to make that.");
        if (proposal.delivery == "mail" && (sObjectMgr.GetItemPrototype(itemEntry)->Flags & ITEM_FLAG_CONJURED))
            return reject("conjured_item_mail_restricted", "That crafted item can't be mailed.");
    }

    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemEntry);
    if (!proto) return reject("unknown_item", "That item is no longer valid.");
    uint32 value = (conjure || buying) ? 0 : proposal.quantity * ItemUsageValue::GetBotSellPrice(proto, bot);
    uint32 minimumUnitPrice = 0, maximumUnitPrice = 0;
    if (quote)
    {
        minimumUnitPrice = quote->limitPrice / std::max<uint32>(1, quote->quantity);
        maximumUnitPrice = quote->openingPrice / std::max<uint32>(1, quote->quantity);
    }
    else if (offeredCapability)
    {
        minimumUnitPrice = offeredCapability->minimumUnitPriceCopper;
        maximumUnitPrice = offeredCapability->maximumUnitPriceCopper;
    }
    else if (buying)
    {
        uint32 unitPrice = ItemUsageValue::GetBotBuyPrice(proto, bot);
        minimumUnitPrice = std::max<uint32>(1, unitPrice / 2);
        maximumUnitPrice = unitPrice;
    }
    else
    {
        uint32 unitPrice = ItemUsageValue::GetBotSellPrice(proto, bot);
        uint32 marketPrice = ItemUsageValue::GetAHMedianBuyoutPricePerItem(proto);
        uint32 vendorPrice = proto->SellPrice;
        minimumUnitPrice = std::max<uint32>(vendorPrice + std::max<uint32>(1, vendorPrice / 10),
            std::max<uint32>(1, unitPrice * 35 / 100));
        maximumUnitPrice = std::max<uint32>(unitPrice * 5, marketPrice * 2);
    }
    uint32 price = 0;
    if (quote)
    {
        // The quote was already bounded and committed on the world thread.
        // Reuse that exact total instead of independently recalculating a
        // per-unit range that can disagree because of integer rounding.
        price = proposal.type == "accept_player_gift" ? 0 : proposal.priceCopper;
    }
    else if (buying)
    {
        uint64 minimum = uint64(minimumUnitPrice) * proposal.quantity;
        uint64 maximum = uint64(maximumUnitPrice) * proposal.quantity;
        uint64 negotiated = proposal.priceCopper ? proposal.priceCopper :
            uint64(proposal.quantity) * ItemUsageValue::GetBotBuyPrice(proto, bot);
        if (!negotiated || negotiated < minimum || negotiated > maximum || negotiated > UINT32_MAX)
            return reject("invalid_price", "That price is outside the valid offer.");
        price = (uint32)negotiated;
    }
    else if (crafting)
    {
        uint32 authoritativeFee = offeredCapability ? offeredCapability->priceCopper : 0;
        if (proposal.priceCopper && proposal.priceCopper != authoritativeFee)
            return reject("craft_fee_changed", "That crafting fee no longer matches the offer.");
        price = authoritativeFee;
    }
    else if (proposal.type == "sell_item")
    {
        uint64 minimum = uint64(minimumUnitPrice) * proposal.quantity;
        uint64 maximum = uint64(maximumUnitPrice) * proposal.quantity;
        uint64 negotiated = proposal.priceCopper ? proposal.priceCopper : value;
        if (!negotiated || negotiated < minimum || negotiated > maximum || negotiated > UINT32_MAX)
            return reject("invalid_price", "That price is outside the valid offer.");
        price = (uint32)negotiated;
    }
    uint32 freeMoney = buying ? bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<uint32>(
        "free money for", std::to_string((uint32)NeedMoneyFor::anything))->Get() : 0;
    if (buying && CountBrokerPlayerItem(player, itemEntry) < proposal.quantity)
        return reject("insufficient_player_inventory", "You no longer have that many to sell.");
    if (buying && offeredCapability && offeredCapability->desiredQuantity)
    {
        uint32 currentQuantity = CountBrokerBotItem(bot, itemEntry);
        uint32 missingQuantity = offeredCapability->desiredQuantity > currentQuantity ?
            offeredCapability->desiredQuantity - currentQuantity : 0;
        if (!missingQuantity)
            return reject("buyer_demand_satisfied", "I don't need any more of that now.");
        if (proposal.quantity > missingQuantity)
            return reject("buyer_quantity_changed", "I don't need that many anymore.");
    }
    if (buying && proposal.type != "accept_player_gift" && !price)
        return reject("invalid_price", "That purchase needs a valid price.");
    if (buying && reservedMoney[bot->GetGUIDLow()] + price > freeMoney)
        return reject("insufficient_bot_spendable_money", "I can't afford that from my available spending money.");
    if (proposal.type == "give_item" || conjure)
    {
        auto& history = giftHistory[proposal.targetGuid];
        const auto cutoff = std::chrono::steady_clock::now() - std::chrono::hours(1);
        history.erase(std::remove_if(history.begin(), history.end(), [&](const auto& stamp) { return stamp < cutoff; }), history.end());
        if (proposal.quantity > 5 || value > 100 || history.size() >= 3 || !offeredCapability->giftEligible)
            return reject("gift_policy_rejected", "I can't give that item away right now.");
    }

    std::string transactionId = "wow-tx-" + event.eventId + "-" + proposal.proposalId;
    if (proposal.delivery == "meeting" && !conjure)
    {
        PlayerbotRendezvousManager::RequestResult meeting =
            sPlayerbotRendezvousManager.Request(bot, player, transactionId, true);
        if (meeting == PlayerbotRendezvousManager::RequestResult::unavailable)
            return reject("meeting_unavailable", "I can't get away from what I'm doing quickly enough right now.");
        if (meeting == PlayerbotRendezvousManager::RequestResult::unsafe)
            return reject("meeting_not_viable", "I can't reach you naturally from here. Let's use mail or meet at a landmark.");
    }

    if (!conjure && !crafting && !buying && proposal.delivery == "mail" && item->GetCount() != proposal.quantity)
    {
        item = SplitBrokerItem(bot, item, proposal.quantity);
        if (!item)
            return reject("inventory_split_failed", "I couldn't prepare that item for mail.");
        itemGuid = item->GetGUIDLow();
    }
    if (proposal.type == "give_item" || conjure)
        giftHistory[proposal.targetGuid].push_back(std::chrono::steady_clock::now());

    Transaction transaction;
    transaction.transactionId = transactionId;
    transaction.commissionId = crafting ? "lwc-" + std::to_string(std::hash<std::string>{}(transactionId)) : "";
    transaction.eventId = event.eventId;
    transaction.proposalId = proposal.proposalId;
    transaction.botGuid = proposal.botGuid;
    transaction.playerGuid = proposal.targetGuid;
    transaction.itemEntry = itemEntry;
    transaction.itemGuid = itemGuid;
    transaction.spellId = spellId;
    transaction.quantity = proposal.quantity;
    transaction.priceCopper = price;
    transaction.type = proposal.type;
    transaction.delivery = proposal.delivery;
    transaction.state = (conjure || crafting) ? "preparing" : (proposal.delivery == "mail" ? "mail_travel" : (proposal.delivery == "meeting" ? "meeting" : "offered"));
    transaction.preparingSince = std::chrono::steady_clock::now();
    transaction.expires = std::chrono::steady_clock::now() + std::chrono::seconds(proposal.delivery == "mail" ? 1800 : (proposal.delivery == "meeting" ? 300 : 60));
    transactions[transaction.transactionId] = transaction;
    if (itemGuid) ReserveItem(itemGuid, transaction.transactionId);
    if (buying) reservedMoney[bot->GetGUIDLow()] += price;
    if (quote) quote->state = "accepted";
    if (crafting)
        CharacterDatabase.PExecute(
            "INSERT INTO organic_economy_commission (commission_id,bot_guid,player_guid,recipe_spell_id,output_item_entry,quantity,materials_source,service_fee_copper,state,authoritative_payload,expires_at) "
            "VALUES ('%s','%u','%u','%u','%u','1','bot','%u','crafting','{}',DATE_ADD(NOW(),INTERVAL 30 MINUTE))",
            transaction.commissionId.c_str(), transaction.botGuid, transaction.playerGuid, transaction.spellId,
            transaction.itemEntry, transaction.priceCopper);
    Report(transactions[transaction.transactionId]);

    if (conjure)
        bot->GetPlayerbotAI()->DoSpecificAction("conjure water", Event("chat action conjure", "", player), true);
    else if (crafting && !bot->GetPlayerbotAI()->CastSpell(spellId, bot))
    {
        Transaction& active = transactions[transaction.transactionId];
        active.state = "failed";
        active.failureReason = "craft cast could not start";
        Report(active);
    }
    else if (proposal.delivery == "direct")
    {
        bot->GetPlayerbotAI()->StopMoving();
        WorldPacket packet(CMSG_INITIATE_TRADE);
        packet << player->GetObjectGuid();
        bot->GetSession()->HandleInitiateTradeOpcode(packet);
        if (bot->GetTradeData() && bot->GetTrader() == player)
            PopulateTrade(bot, player);
    }
    else if (proposal.delivery == "mail")
    {
        std::ostringstream action;
        action << "request travel target::" << (uint32)TravelDestinationPurpose::Mail;
        bot->GetPlayerbotAI()->DoSpecificAction(action.str(), Event("chat action mail", "", player), true);
    }
    else if (proposal.delivery == "meeting")
    {
        Transaction& active = transactions[transaction.transactionId];
        if (bot->IsWithinDistInMap(player, INTERACTION_DISTANCE))
        {
            active.state = "offered";
            bot->GetPlayerbotAI()->StopMoving();
            WorldPacket packet(CMSG_INITIATE_TRADE);
            packet << player->GetObjectGuid();
            bot->GetSession()->HandleInitiateTradeOpcode(packet);
            Report(active);
            if (bot->GetTradeData() && bot->GetTrader() == player)
                PopulateTrade(bot, player);
        }
        else
            active.lastMeetingMove = std::chrono::steady_clock::now();
    }
    return PlayerbotActionResult(true, "created", "");
}

bool PlayerbotActionBroker::Authorizes(Player* bot, Player* trader) const
{
    return bot && trader && Find(bot->GetGUIDLow(), trader->GetGUIDLow()) != nullptr;
}

uint32 PlayerbotActionBroker::ReservedCopper(uint32 botGuid) const
{
    const auto found = reservedMoney.find(botGuid);
    return found == reservedMoney.end() ? 0 : found->second;
}

bool PlayerbotActionBroker::IsItemReserved(uint32 itemGuid) const
{
    return ReservedItemsView()->Item(itemGuid) || sGuildSupplies.Reserved(itemGuid);
}

void PlayerbotActionBroker::ReserveItem(uint32 guid,const std::string& transaction)
{
    if (!guid) return;
    itemProtection.SetItem(guid,true);
    reservedItems[guid]=transaction;
}

void PlayerbotActionBroker::ReleaseItem(uint32 guid)
{
    reservedItems.erase(guid);
    itemProtection.SetItem(guid,false);
}

bool PlayerbotActionBroker::PopulateTrade(Player* bot, Player* trader)
{
    Transaction* transaction = bot && trader ? Find(bot->GetGUIDLow(), trader->GetGUIDLow()) : nullptr;
    if (!transaction || !bot->GetTradeData() || bot->GetTrader() != trader)
        return false;
    TradeData* trade = bot->GetTradeData();
    if (transaction->type == "buy_item" || transaction->type == "accept_player_gift")
    {
        if (bot->GetMoney() < transaction->priceCopper)
        {
            CancelTrade(bot, trader, "reserved funds became unavailable");
            return false;
        }
        trade->SetMoney(transaction->priceCopper);
        if (trade->GetMoney() != transaction->priceCopper)
            return false;
        trader->GetSession()->SendUpdateTrade(true);
        transaction->state = "trading";
        transaction->failureReason.clear();
        Report(*transaction);
        return true;
    }
    Item* offered = bot->GetTradeData()->GetItem((TradeSlots)0);
    if (offered && offered->GetGUIDLow() == transaction->itemGuid && offered->GetCount() == transaction->quantity)
        return true;
    Item* item = FindBrokerItem(bot, transaction->itemEntry, transaction->itemGuid);
    if (!item || item->GetCount() < transaction->quantity || !item->CanBeTraded())
    {
        FindItemByIdVisitor visitor(transaction->itemEntry);
        bot->GetPlayerbotAI()->InventoryIterateItems(&visitor, IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
        for (Item* candidate : visitor.GetResult())
        {
            auto reservation = reservedItems.find(candidate->GetGUIDLow());
            if (candidate->GetCount() >= transaction->quantity && candidate->CanBeTraded() &&
                !sGuildSupplies.Reserved(candidate->GetGUIDLow()) &&
                (reservation == reservedItems.end() || reservation->second == transaction->transactionId))
            {
                ReleaseItem(transaction->itemGuid);
                item = candidate;
                transaction->itemGuid = candidate->GetGUIDLow();
                ReserveItem(transaction->itemGuid, transaction->transactionId);
                break;
            }
        }
    }
    if (!item || item->GetCount() < transaction->quantity || !item->CanBeTraded())
    {
        CancelTrade(bot, trader, "reserved item is no longer available");
        return false;
    }
    if (item->GetCount() != transaction->quantity)
    {
        Item* split = SplitBrokerItem(bot, item, transaction->quantity);
        if (!split)
        {
            CancelTrade(bot, trader, "could not prepare the promised quantity");
            return false;
        }
        ReleaseItem(transaction->itemGuid);
        item = split;
        transaction->itemGuid = split->GetGUIDLow();
        ReserveItem(transaction->itemGuid, transaction->transactionId);
    }

    // Bot-initiated trades create TradeData before the human has opened the window.
    // Keep the authoritative slot populated now; Update() will resend it after the
    // window opens so the TBC client cannot miss this first extended trade update.
    trade->SetItem((TradeSlots)0, item);
    trader->GetSession()->SendUpdateTrade(true);
    offered = trade->GetItem((TradeSlots)0);
    if (!offered || offered->GetGUIDLow() != transaction->itemGuid || offered->GetCount() != transaction->quantity)
    {
        if (transaction->failureReason != "server could not populate the promised trade item")
        {
            transaction->failureReason = "server could not populate the promised trade item";
            Report(*transaction);
        }
        return false;
    }

    transaction->state = "trading";
    transaction->failureReason.clear();
    Report(*transaction);
    return true;
}

bool PlayerbotActionBroker::ValidateTrade(Player* bot, Player* trader)
{
    Transaction* transaction = bot && trader ? Find(bot->GetGUIDLow(), trader->GetGUIDLow()) : nullptr;
    if (!transaction || !bot->GetTradeData() || !trader->GetTradeData())
        return false;
    if (transaction->type == "buy_item" || transaction->type == "accept_player_gift")
    {
        uint32 received = 0;
        for (uint32 slot = 0; slot < TRADE_SLOT_TRADED_COUNT; ++slot)
        {
            if (bot->GetTradeData()->GetItem((TradeSlots)slot)) return false;
            Item* item = trader->GetTradeData()->GetItem((TradeSlots)slot);
            if (item)
            {
                if (item->GetEntry() != transaction->itemEntry || !item->CanBeTraded()) return false;
                received += item->GetCount();
            }
        }
        return received == transaction->quantity && bot->GetTradeData()->GetMoney() == transaction->priceCopper &&
            trader->GetTradeData()->GetMoney() == 0;
    }
    Item* offered = bot->GetTradeData()->GetItem((TradeSlots)0);
    if (!offered || offered->GetGUIDLow() != transaction->itemGuid || offered->GetCount() != transaction->quantity)
        return false;
    ItemPosCountVec destination;
    uint8 bagSlot = 0;
    if (trader->CanStoreItem(NULL_BAG, NULL_SLOT, destination, offered, bagSlot, false) != EQUIP_ERR_OK)
    {
        if (transaction->failureReason != "player inventory has no room")
        {
            transaction->failureReason = "player inventory has no room";
            bot->GetPlayerbotAI()->Whisper(
                "Your bags look full. Make some room and try accepting again.",
                trader->GetName(), false, PlayerbotAI::ChatMessageClass::social);
            Report(*transaction);
        }
        return false;
    }
    for (uint32 slot = 1; slot < TRADE_SLOT_TRADED_COUNT; ++slot)
        if (bot->GetTradeData()->GetItem((TradeSlots)slot))
            return false;
    for (uint32 slot = 0; slot < TRADE_SLOT_TRADED_COUNT; ++slot)
        if (trader->GetTradeData()->GetItem((TradeSlots)slot))
            return false;
    return bot->GetTradeData()->GetMoney() == 0 && trader->GetTradeData()->GetMoney() == transaction->priceCopper;
}

static Item* FindBrokerItemStack(Player* bot, uint32 itemEntry, uint32 quantity)
{
    FindItemByIdVisitor visitor(itemEntry);
    bot->GetPlayerbotAI()->InventoryIterateItems(&visitor, IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
    for (Item* item : visitor.GetResult())
        if (item->GetCount() == quantity && item->CanBeTraded() && !item->IsInTrade())
            return item;
    return nullptr;
}

static uint32 CountBrokerPlayerItem(Player* player, uint32 entry)
{
    uint32 count = 0;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            if (item->GetEntry() == entry && item->CanBeTraded() && !item->IsSoulBound()) count += item->GetCount();
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = (Bag*)player->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                if (Item* item = bag->GetItemByPos(slot))
                    if (item->GetEntry() == entry && item->CanBeTraded() && !item->IsSoulBound()) count += item->GetCount();
    return count;
}

void PlayerbotActionBroker::CompleteTrade(Player* bot, Player* trader)
{
    Transaction* transaction = bot && trader ? Find(bot->GetGUIDLow(), trader->GetGUIDLow()) : nullptr;
    if (!transaction) return;
    transaction->state = "completed";
    ReleaseItem(transaction->itemGuid);
    if (transaction->type == "buy_item" || transaction->type == "accept_player_gift") reservedMoney[transaction->botGuid] -= std::min(reservedMoney[transaction->botGuid], transaction->priceCopper);
    Report(*transaction);
    sPlayerbotRendezvousManager.BeginDeparture(transaction->botGuid, transaction->playerGuid, "trade_completed");
}

void PlayerbotActionBroker::CancelTrade(Player* bot, Player* trader, const std::string& reason)
{
    Transaction* transaction = bot && trader ? Find(bot->GetGUIDLow(), trader->GetGUIDLow()) : nullptr;
    if (!transaction) return;
    transaction->state = "cancelled";
    transaction->failureReason = reason;
    ReleaseItem(transaction->itemGuid);
    if (transaction->type == "buy_item" || transaction->type == "accept_player_gift") reservedMoney[transaction->botGuid] -= std::min(reservedMoney[transaction->botGuid], transaction->priceCopper);
    Report(*transaction);
    sPlayerbotRendezvousManager.BeginDeparture(transaction->botGuid, transaction->playerGuid, reason);
}

void PlayerbotActionBroker::CancelTrade(Player* bot, const std::string& reason)
{
    if (!bot) return;
    Transaction* transaction = nullptr;
    for (auto& pair : transactions)
        if (pair.second.botGuid == bot->GetGUIDLow() && pair.second.state != "completed" &&
            pair.second.state != "cancelled" && pair.second.state != "expired" && pair.second.state != "failed")
        {
            transaction = &pair.second;
            break;
        }
    if (!transaction) return;
    transaction->state = "cancelled";
    transaction->failureReason = reason;
    ReleaseItem(transaction->itemGuid);
    if (transaction->type == "buy_item" || transaction->type == "accept_player_gift") reservedMoney[transaction->botGuid] -= std::min(reservedMoney[transaction->botGuid], transaction->priceCopper);
    Report(*transaction);
    sPlayerbotRendezvousManager.BeginDeparture(transaction->botGuid, transaction->playerGuid, reason);
}

void PlayerbotActionBroker::Update()
{
    sPlayerbotRendezvousManager.Update();
    const auto now = std::chrono::steady_clock::now();
    for (auto& pair : transactions)
    {
        Transaction& transaction = pair.second;
        if (transaction.state != "preparing" && transaction.state != "mail_travel" && transaction.state != "meeting" && transaction.state != "offered" && transaction.state != "trading")
            continue;
        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(transaction.botGuid);
        Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, transaction.playerGuid));
        bool buying = transaction.type == "buy_item" || transaction.type == "accept_player_gift";
        Item* item = bot && transaction.itemGuid ? FindBrokerItem(bot, transaction.itemEntry, transaction.itemGuid) : nullptr;
        if (bot && !buying && transaction.state != "preparing" &&
            (!item || item->GetCount() < transaction.quantity || !item->CanBeTraded()))
        {
            FindItemByIdVisitor visitor(transaction.itemEntry);
            bot->GetPlayerbotAI()->InventoryIterateItems(&visitor, IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
            for (Item* candidate : visitor.GetResult())
            {
                auto reservation = reservedItems.find(candidate->GetGUIDLow());
                if (candidate->GetCount() >= transaction.quantity && candidate->CanBeTraded() &&
                    !sGuildSupplies.Reserved(candidate->GetGUIDLow()) &&
                    (reservation == reservedItems.end() || reservation->second == transaction.transactionId))
                {
                    ReleaseItem(transaction.itemGuid);
                    item = candidate;
                    transaction.itemGuid = candidate->GetGUIDLow();
                    ReserveItem(transaction.itemGuid, transaction.transactionId);
                    break;
                }
            }
        }
        // NearTeleportTo updates map position immediately, while the cached zone
        // can remain the bot's pre-relocation zone until the next area update.
        // Do not tear down a validated, active rendezvous during that window.
        // Initial transaction creation still requires the same zone, and a map
        // change remains an immediate cancellation condition.
        bool rendezvousActive = bot && player &&
            sPlayerbotRendezvousManager.IsActive(transaction.botGuid, transaction.playerGuid);
        bool incompatible = transaction.delivery != "mail" && bot && player &&
            (bot->GetMapId() != player->GetMapId() ||
                (!rendezvousActive && bot->GetZoneId() != player->GetZoneId()));
        if (!bot || !player || !bot->IsAlive() || !player->IsAlive() || incompatible ||
            (buying && CountBrokerPlayerItem(player, transaction.itemEntry) < transaction.quantity) ||
            (!buying && transaction.state != "preparing" && (!item || item->GetCount() < transaction.quantity || !item->CanBeTraded())))
        {
            transaction.state = "cancelled";
            if (!bot || !player || !bot->IsAlive() || !player->IsAlive())
                transaction.failureReason = "participant became unavailable";
            else if (incompatible)
                transaction.failureReason = "participants are no longer in the same zone";
            else if (!buying && !item)
                transaction.failureReason = "reserved item is no longer in inventory";
            else if (!buying && item->GetCount() < transaction.quantity)
                transaction.failureReason = "reserved item quantity changed";
            else if (!buying && !item->CanBeTraded())
                transaction.failureReason = "reserved item is no longer tradeable";
            else
                transaction.failureReason = "requested trade resources became unavailable";
            ReleaseItem(transaction.itemGuid);
            if (buying) reservedMoney[transaction.botGuid] -= std::min(reservedMoney[transaction.botGuid], transaction.priceCopper);
            Report(transaction);
            sPlayerbotRendezvousManager.BeginDeparture(transaction.botGuid, transaction.playerGuid, transaction.failureReason);
            continue;
        }
        if (transaction.state == "preparing")
        {
            bool crafting = transaction.type == "craft_commission";
            Item* created = FindBrokerItemStack(bot, transaction.itemEntry, transaction.quantity);
            if (created)
            {
                transaction.itemGuid = created->GetGUIDLow();
                ReserveItem(transaction.itemGuid, transaction.transactionId);
                if (transaction.delivery == "direct" && bot->IsWithinDistInMap(player, INTERACTION_DISTANCE))
                {
                    transaction.state = "offered";
                    WorldPacket packet(CMSG_INITIATE_TRADE);
                    packet << player->GetObjectGuid();
                    bot->GetSession()->HandleInitiateTradeOpcode(packet);
                    bot->GetPlayerbotAI()->Whisper(
                        crafting ? "It's ready. Open trade." : "Water is ready. Open trade.",
                        player->GetName(), false, PlayerbotAI::ChatMessageClass::social);
                }
                else
                {
                    transaction.state = "meeting";
                    PlayerbotRendezvousManager::RequestResult meeting = sPlayerbotRendezvousManager.Request(
                        bot, player, transaction.transactionId, true);
                    if (meeting == PlayerbotRendezvousManager::RequestResult::accepted ||
                        meeting == PlayerbotRendezvousManager::RequestResult::ordinary_travel)
                    {
                        transaction.lastMeetingMove = now;
                        bot->GetPlayerbotAI()->Whisper(
                            crafting ? "It's ready. I'm heading to you." :
                                "Water is ready. I'm heading to you.",
                            player->GetName(), false, PlayerbotAI::ChatMessageClass::social);
                    }
                else if (transaction.delivery == "mail")
                {
                    transaction.state = "mail_travel";
                    std::ostringstream action;
                    action << "request travel target::" << (uint32)TravelDestinationPurpose::Mail;
                    bot->GetPlayerbotAI()->DoSpecificAction(action.str(), Event("chat action commission mail", "", player), true);
                    bot->GetPlayerbotAI()->Whisper(
                        "It's ready. I'll mail it once I reach a mailbox.", player->GetName(),
                        false, PlayerbotAI::ChatMessageClass::social);
                }
                    else
                        bot->GetPlayerbotAI()->Whisper(
                            crafting ? "It's ready, but I can't head over right now." :
                                "Water is ready, but I can't head over right now.",
                            player->GetName(), false, PlayerbotAI::ChatMessageClass::social);
                }
                Report(transaction);
            }
            else if (std::chrono::duration_cast<std::chrono::seconds>(now - transaction.preparingSince).count() >= 12)
            {
                transaction.state = "failed";
                transaction.failureReason = crafting ? "crafting did not produce the promised item" : "conjuring did not produce the promised stack";
                bot->GetPlayerbotAI()->Whisper(
                    crafting ? "I couldn't finish that craft after all, sorry." :
                        "Couldn't conjure that water after all, sorry.",
                    player->GetName(), false, PlayerbotAI::ChatMessageClass::social);
                Report(transaction);
            }
            continue;
        }
        if (transaction.state == "mail_travel")
        {
            ObjectGuid mailbox = MailProcessor::FindMailbox(bot->GetPlayerbotAI());
            if (mailbox && bot->GetMoney() >= 30)
            {
                WorldPacket packet(CMSG_SEND_MAIL);
                packet << mailbox << player->GetName();
                packet << std::string(transaction.priceCopper ? "Living TBC sale" : "Living TBC gift");
                packet << std::string("An item promised in chat.");
                packet << (uint32)0 << (uint32)0 << (uint8)1;
                packet << (uint8)0 << item->GetObjectGuid();
                packet << (uint32)0 << transaction.priceCopper << (uint64)0 << (uint8)0;
                bot->GetSession()->HandleSendMail(packet);
                if (!FindBrokerItem(bot, transaction.itemEntry, transaction.itemGuid))
                {
                    transaction.state = "completed";
                    ReleaseItem(transaction.itemGuid);
                }
                else
                {
                    transaction.state = "failed";
                    transaction.failureReason = "mail draft was rejected";
                    ReleaseItem(transaction.itemGuid);
                }
                Report(transaction);
                continue;
            }
        }
        if (transaction.state == "meeting" && !bot->IsWithinDistInMap(player, INTERACTION_DISTANCE) &&
            !bot->IsInCombat() && (transaction.lastMeetingMove.time_since_epoch().count() == 0 ||
            std::chrono::duration_cast<std::chrono::seconds>(now - transaction.lastMeetingMove).count() >= 1))
        {
            if (!sPlayerbotRendezvousManager.IsActive(transaction.botGuid, transaction.playerGuid))
            {
                PlayerbotRendezvousManager::RequestResult meeting = sPlayerbotRendezvousManager.Request(
                    bot, player, transaction.transactionId, true);
                if (meeting == PlayerbotRendezvousManager::RequestResult::accepted ||
                    meeting == PlayerbotRendezvousManager::RequestResult::ordinary_travel)
                    transaction.lastMeetingMove = now;
            }
        }
        if (transaction.state == "meeting" && !bot->IsInCombat() && !player->IsInCombat() &&
            bot->IsWithinDistInMap(player, INTERACTION_DISTANCE))
        {
            transaction.state = "offered";
            bot->GetPlayerbotAI()->StopMoving();
            WorldPacket packet(CMSG_INITIATE_TRADE);
            packet << player->GetObjectGuid();
            bot->GetSession()->HandleInitiateTradeOpcode(packet);
            transaction.lastTradeAttempt = now;
            ++transaction.tradeOpenAttempts;
            Report(transaction);
            if (bot->GetTradeData() && bot->GetTrader() == player)
                PopulateTrade(bot, player);
        }
        if (transaction.state == "offered" && bot->GetTradeData() && bot->GetTrader() == player)
            PopulateTrade(bot, player);
        if (transaction.state == "offered" || transaction.state == "trading")
        {
            // Arrival is not transaction completion. Keep the bot parked even
            // if CMaNGOS briefly clears Trader/TradeData while the client opens
            // or closes the window; otherwise ordinary quest AI can carry the
            // bot away while the broker still owns a live transaction.
            if (!bot->IsInCombat() && bot->IsWithinDistInMap(player, INTERACTION_DISTANCE))
                bot->GetPlayerbotAI()->StopMoving();

            bool liveWindow = bot->GetTrader() == player && bot->GetTradeData() && player->GetTradeData();
            bool retryDue = transaction.lastTradeAttempt.time_since_epoch().count() == 0 ||
                std::chrono::duration_cast<std::chrono::seconds>(now - transaction.lastTradeAttempt).count() >= 3;
            if (!liveWindow && !bot->IsInCombat() && !player->IsInCombat() &&
                bot->IsWithinDistInMap(player, INTERACTION_DISTANCE) && retryDue &&
                transaction.tradeOpenAttempts < 3)
            {
                transaction.state = "offered";
                WorldPacket packet(CMSG_INITIATE_TRADE);
                packet << player->GetObjectGuid();
                bot->GetSession()->HandleInitiateTradeOpcode(packet);
                transaction.lastTradeAttempt = now;
                ++transaction.tradeOpenAttempts;
                Report(transaction);
                if (bot->GetTradeData() && bot->GetTrader() == player)
                    PopulateTrade(bot, player);
            }
        }
        if ((transaction.state == "mail_travel" || transaction.state == "meeting" || transaction.state == "offered" || transaction.state == "trading") && now >= transaction.expires)
        {
            transaction.state = "expired";
            transaction.failureReason = "transaction expired";
            ReleaseItem(transaction.itemGuid);
            if (buying) reservedMoney[transaction.botGuid] -= std::min(reservedMoney[transaction.botGuid], transaction.priceCopper);
            Report(transaction);
            sPlayerbotRendezvousManager.BeginDeparture(transaction.botGuid, transaction.playerGuid, "transaction_expired");
        }
    }
}

void PlayerbotActionBroker::ReportRejected(const ChatDirectorActionProposal& proposal,
    const ChatDirectorEvent& event, const std::string& reason) const
{
    Transaction transaction;
    transaction.transactionId = "wow-rejected-" + event.eventId + "-" + proposal.proposalId;
    transaction.eventId = event.eventId;
    transaction.proposalId = proposal.proposalId;
    transaction.botGuid = proposal.botGuid;
    transaction.playerGuid = proposal.targetGuid;
    transaction.quantity = proposal.quantity;
    transaction.priceCopper = proposal.priceCopper;
    transaction.type = proposal.type;
    transaction.delivery = proposal.delivery;
    transaction.state = "rejected";
    transaction.failureReason = reason;

    std::smatch match;
    if (std::regex_match(proposal.capabilityRef, match, std::regex(R"(buy:([0-9]+))")))
        transaction.itemEntry = (uint32)std::stoul(match[1].str());
    else if (std::regex_match(proposal.capabilityRef, match, std::regex(R"(item:([0-9]+):([0-9]+))")))
    {
        transaction.itemEntry = (uint32)std::stoul(match[1].str());
        transaction.itemGuid = (uint32)std::stoul(match[2].str());
    }
    else if (std::regex_match(proposal.capabilityRef, match,
        std::regex(R"(spell:conjure_water:([0-9]+):([0-9]+))")))
        transaction.itemEntry = (uint32)std::stoul(match[2].str());
    else if (std::regex_match(proposal.capabilityRef, match,
        std::regex(R"(spell:craft:([0-9]+):([0-9]+))")))
        transaction.itemEntry = (uint32)std::stoul(match[2].str());
    Report(transaction);
}

void PlayerbotActionBroker::Report(const Transaction& transaction) const
{
    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(transaction.botGuid);
    Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, transaction.playerGuid));
    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(transaction.itemEntry);
    std::ostringstream body;
    body << "{\"transaction_id\":\"" << PlayerbotLLMInterface::SanitizeForJson(transaction.transactionId)
         << "\",\"event_id\":\"" << PlayerbotLLMInterface::SanitizeForJson(transaction.eventId)
         << "\",\"proposal_id\":\"" << PlayerbotLLMInterface::SanitizeForJson(transaction.proposalId)
         << "\",\"bot_guid\":" << transaction.botGuid << ",\"bot_name\":\""
         << PlayerbotLLMInterface::SanitizeForJson(bot ? bot->GetName() : "") << "\",\"player_guid\":" << transaction.playerGuid
         << ",\"player_name\":\"" << PlayerbotLLMInterface::SanitizeForJson(player ? player->GetName() : "")
         << "\",\"type\":\"" << transaction.type << "\",\"capability_ref\":\"item:" << transaction.itemEntry << ':' << transaction.itemGuid
         << "\",\"item_name\":\"" << PlayerbotLLMInterface::SanitizeForJson(proto ? proto->Name1 : "")
         << "\",\"quantity\":" << transaction.quantity << ",\"price_copper\":" << transaction.priceCopper
         << ",\"delivery\":\"" << transaction.delivery << "\",\"state\":\"" << transaction.state
         << "\",\"rendezvous_state\":\"" << sPlayerbotRendezvousManager.State(transaction.botGuid, transaction.playerGuid)
         << "\",\"catchup_relocated\":" << (sPlayerbotRendezvousManager.WasRelocated(transaction.botGuid, transaction.playerGuid) ? "true" : "false")
         << ",\"failure_reason\":\"" << PlayerbotLLMInterface::SanitizeForJson(transaction.failureReason)
         << "\",\"expires_at\":\"world-clock\"}";
    std::string payload = body.str();
    std::thread([payload]() {
        std::vector<std::string> debug;
        PlayerbotLLMInterface::Generate(payload, 3, 2, debug, true, "/v2/action-status");
    }).detach();
    if (transaction.type == "craft_commission" && !transaction.commissionId.empty())
    {
        std::string commissionState = transaction.state;
        if (commissionState == "preparing") commissionState = "crafting";
        else if (commissionState == "meeting" || commissionState == "mail_travel") commissionState = "traveling";
        else if (commissionState == "offered" || commissionState == "trading") commissionState = "ready";
        CharacterDatabase.PExecute(
            "UPDATE organic_economy_commission SET state='%s',failure_reason='' WHERE commission_id='%s'",
            commissionState.c_str(), transaction.commissionId.c_str());
        std::ostringstream commission;
        commission << "{\"events\":[{\"event_id\":\"commission-"
            << PlayerbotLLMInterface::SanitizeForJson(transaction.commissionId + '-' + commissionState)
            << "\",\"type\":\"commission\",\"commission_id\":\""
            << PlayerbotLLMInterface::SanitizeForJson(transaction.commissionId)
            << "\",\"bot_guid\":" << transaction.botGuid << ",\"player_guid\":" << transaction.playerGuid
            << ",\"recipe_id\":" << transaction.spellId << ",\"output_item\":" << transaction.itemEntry
            << ",\"output_name\":\"" << PlayerbotLLMInterface::SanitizeForJson(proto ? proto->Name1 : "")
            << "\",\"quantity\":" << transaction.quantity << ",\"fee_copper\":" << transaction.priceCopper
            << ",\"materials_source\":\"bot\",\"state\":\"" << commissionState
            << "\",\"failure_reason\":\"" << PlayerbotLLMInterface::SanitizeForJson(transaction.failureReason)
            << "\",\"expires_at\":" << (uint32(time(nullptr)) + 1800) << "}]}";
        std::string commissionPayload = commission.str();
        std::thread([commissionPayload]() { std::vector<std::string> debug;
            PlayerbotLLMInterface::Generate(commissionPayload, 3, 2, debug, true, "/v2/economy-events"); }).detach();
    }
}
