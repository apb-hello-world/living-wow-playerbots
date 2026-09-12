#include "playerbot/LivingAuctionBid.h"

#include "playerbot/playerbot.h"
#include "playerbot/LivingActivityCoordinator.h"
#include "playerbot/LivingPurchaseBudget.h"
#include "playerbot/LivingProfessionDemand.h"
#include "playerbot/LivingNativeAuctionPurchase.h"
#include "playerbot/LivingUsefulRecipe.h"
#include "playerbot/PlayerbotAuctionEligibility.h"
#include "playerbot/PlayerbotServiceTracking.h"
#include "AhAction.h"
#include "playerbot/PlayerbotActionBroker.h"
#include "playerbot/PlayerbotGuildSupplies.h"
#include "playerbot/PlayerbotOrganicEconomy.h"
#include "Mails/Mail.h"
#include <mutex>
#include <tuple>
#include <limits>
#include <set>
#include "playerbot/strategy/values/ItemCountValue.h"
#include "playerbot/RandomItemMgr.h"
#include "playerbot/strategy/values/BudgetValues.h"
#include "playerbot/strategy/values/ItemUsageValue.h"
#include <chrono>
#include <fstream>
#include <regex>
#include <sstream>

using namespace ai;

namespace
{
    struct OrganicAuctionPolicy
    {
        std::string mode = "observe";
        bool posting = false;
        bool buying = false;
        uint32 humanPreference = 5;
        uint32 maxPurchasesPerHour = 3;
        uint32 maxDailySpendPercent = 25;
    };

    bool JsonBool(const std::string& source, const std::string& key, bool fallback)
    {
        std::smatch match;
        std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*(true|false)");
        return std::regex_search(source, match, pattern) ? match[1].str() == "true" : fallback;
    }

    uint32 JsonUInt(const std::string& source, const std::string& key, uint32 fallback)
    {
        std::smatch match;
        std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*([0-9]+)");
        return std::regex_search(source, match, pattern) ? uint32(std::stoul(match[1].str())) : fallback;
    }

    OrganicAuctionPolicy GetOrganicAuctionPolicy()
    {
        static std::mutex policyMutex;
        std::lock_guard<std::mutex> guard(policyMutex);
        static OrganicAuctionPolicy policy;
        static std::chrono::steady_clock::time_point loaded;
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (loaded.time_since_epoch().count() && now - loaded < std::chrono::seconds(60))
            return policy;
        loaded = now;
        std::ifstream input("/srv/living-wow/config/economy.json");
        if (!input)
            return policy;
        std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::smatch mode;
        if (std::regex_search(source, mode, std::regex("\\\"mode\\\"\\s*:\\s*\\\"(off|observe|active)\\\"")))
            policy.mode = mode[1].str();
        policy.posting = JsonBool(source, "characterAuctionPosting", false);
        policy.buying = JsonBool(source, "characterAuctionBuying", false);
        policy.humanPreference = std::min<uint32>(5, JsonUInt(source, "humanListingPreferencePercent", 5));
        policy.maxPurchasesPerHour = std::min<uint32>(20, JsonUInt(source, "maximumPurchasesPerBotPerHour", 3));
        policy.maxDailySpendPercent = std::min<uint32>(100, JsonUInt(source, "maximumDiscretionarySpendPercentPerDay", 25));
        return policy;
    }

    uint32 ListingLimit(uint32 level)
    {
        if (level < 20) return 3;
        if (level < 40) return 7;
        if (level < 60) return 12;
        return 20;
    }

    uint32 CharacterAuctionCount(uint32 guid)
    {
        std::unique_ptr<QueryResult> result = CharacterDatabase.PQuery(
            "SELECT COUNT(*) FROM auction WHERE itemowner='%u'", guid);
        return result ? (*result)[0].GetUInt32() : 0;
    }

    uint32 CharacterAccount(uint32 guid)
    {
        static std::map<uint32, uint32> accounts;
        static std::chrono::steady_clock::time_point refreshed;
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (!refreshed.time_since_epoch().count() || now - refreshed > std::chrono::seconds(60))
        {
            accounts.clear();
            refreshed = now;
        }
        std::map<uint32, uint32>::const_iterator found = accounts.find(guid);
        if (found != accounts.end()) return found->second;
        std::unique_ptr<QueryResult> result = CharacterDatabase.PQuery(
            "SELECT account FROM characters WHERE guid='%u'", guid);
        uint32 account = result ? (*result)[0].GetUInt32() : 0;
        accounts[guid] = account;
        return account;
    }

    uint32 RecentPurchases(uint32 guid, uint32 seconds, uint32* spent = nullptr)
    {
        std::unique_ptr<QueryResult> result = CharacterDatabase.PQuery(
            "SELECT COUNT(*),COALESCE(SUM(unit_price_copper*quantity),0) FROM organic_economy_auction_history "
            "WHERE buyer_guid='%u' AND outcome IN ('bid','sold') AND occurred_at>DATE_SUB(NOW(),INTERVAL %u SECOND)", guid, seconds);
        if (!result) { if (spent) *spent = std::numeric_limits<uint32>::max(); return std::numeric_limits<uint32>::max(); }
        if (spent) *spent = (*result)[1].GetUInt32();
        return (*result)[0].GetUInt32();
    }

    LivingActivity::PurchaseSpend RecentSpending(uint32 guid, uint32 seller = 0)
    {
        LivingActivity::PurchaseSpend spend;
        // Baseline/off deployments may not yet have the additive task schema.
        // Preserve their old history path until the coordinator probes it.
        if (!sLivingActivityCoordinator.PurchaseLedgerReady())
        {
            uint32 paid=0;
            const auto dayCount=RecentPurchases(guid,DAY,&paid);
            const auto hourCount=RecentPurchases(guid,HOUR);
            if (dayCount==std::numeric_limits<uint32>::max() || hourCount==std::numeric_limits<uint32>::max())
                return spend;
            spend={true,hourCount,paid,0};
            if (seller)
            {
                std::unique_ptr<QueryResult> pair=CharacterDatabase.PQuery(
                    "SELECT COUNT(*) FROM organic_economy_auction_history WHERE seller_guid='%u' AND buyer_guid='%u' "
                    "AND outcome='sold' AND occurred_at>DATE_SUB(NOW(),INTERVAL 7 DAY)",seller,guid);
                if (!pair || !LivingActivity::DecodeSellerPurchaseCount((*pair)[0].GetCppString(),spend)) return {};
            }
            return spend;
        }
        const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::unique_ptr<QueryResult> result=CharacterDatabase.Query(LivingActivity::PurchaseSpendQuery(guid,now,"",seller).c_str());
        if (result && result->GetFieldCount()==(seller ? 5 : 4))
        {
            auto* f=result->Fetch();
            LivingActivity::DecodePurchaseSpend({f[0].GetCppString(),f[1].GetCppString(),f[2].GetCppString(),f[3].GetCppString()},spend);
            if (seller) LivingActivity::DecodeSellerPurchaseCount(f[4].GetCppString(),spend);
        }
        return spend;
    }

    bool AuctionPurchaseAllowed(const LivingActivity::PurchaseSpend& spend,const OrganicAuctionPolicy& policy,
        uint32 wallet,uint32 available,uint32 price)
    {
        std::string blocker;
        return LivingActivity::WithinPurchaseBudget(spend,{policy.maxPurchasesPerHour,policy.maxDailySpendPercent},
            wallet,available,price,true,blocker);
    }

    struct MaterialOffer { uint32 id, entry, count, price, owner; };
    struct MaterialMarket {
        uint32 refreshed = 0;
        std::map<uint32, std::vector<MaterialOffer>> items;
    };
    const std::vector<MaterialOffer>& MaterialOffers(AuctionHouseObject* house, uint32 entry)
    {
        // Access only under the existing AH mutex. Shared once-per-minute
        // indexing avoids a full auction scan for every recipe and every bot.
        static std::map<AuctionHouseObject*, MaterialMarket> markets;
        static const std::vector<MaterialOffer> empty;
        auto& market = markets[house];
        const uint32 now = uint32(time(nullptr));
        if (!market.refreshed || now >= market.refreshed + 60)
        {
            market.refreshed = now; market.items.clear();
            uint32 examined = 0;
            for (const auto& pair : house->GetAuctions())
            {
                if (++examined > 50000) break; // Bounded even on a misconfigured economy.
                const auto* a = pair.second;
                if (!a || !a->owner || !a->buyout || !a->itemCount || a->expireTime <= time(nullptr)) continue;
                auto& offers = market.items[a->itemTemplate];
                offers.push_back({a->Id, a->itemTemplate, a->itemCount, a->buyout, a->owner});
                std::sort(offers.begin(), offers.end(), [](const MaterialOffer& a, const MaterialOffer& b) {
                    return std::tie(a.count,a.price,a.id) < std::tie(b.count,b.price,b.id);
                });
                if (offers.size() > 16) offers.resize(16);
            }
        }
        auto found = market.items.find(entry);
        return found == market.items.end() ? empty : found->second;
    }
    bool MaterialSeller(Player* bot, uint32 seller)
    {
        const uint32 account = sObjectMgr.GetPlayerAccountIdByGUID(ObjectGuid(HIGHGUID_PLAYER,seller));
        return seller && seller != bot->GetGUIDLow() && account && account != bot->GetSession()->GetAccountId();
    }
}

bool LivingActivity::ValidateNativeProfessionBudget(Player& actor,const Task& saved,const std::string& operation,
    uint32_t price,std::string& blocker)
{
    const auto policy=GetOrganicAuctionPolicy();
    if (policy.mode!="active" || !actor.GetPlayerbotAI()) {blocker="profession_purchasing_policy_disabled"; return false;}
    PurchaseSpend spend;
    if (!sLivingActivityCoordinator.ReadPurchaseBudget(actor.GetGUIDLow(),saved.id,saved.revision,operation,spend,blocker)) return false;
    const uint32 available=actor.GetPlayerbotAI()->GetAiObjectContext()->GetValue<uint32>("free money for",uint32(NeedMoneyFor::tradeskill))->Get();
    return WithinPurchaseBudget(spend,{policy.maxPurchasesPerHour,policy.maxDailySpendPercent},actor.GetMoney(),available,price,false,blocker);
}

bool LivingActivity::ValidateNativeAuctionBudget(Player& actor,const Task& saved,const std::string& operation,
    uint32_t price,uint32_t seller,std::string& blocker)
{
    const auto policy=GetOrganicAuctionPolicy();
    if(policy.mode!="active" || !policy.buying || !actor.GetPlayerbotAI() || !seller) {
        blocker="profession_auction_purchasing_disabled";return false;
    }
    PurchaseSpend spend;
    if(!sLivingActivityCoordinator.ReadPurchaseBudget(actor.GetGUIDLow(),saved.id,saved.revision,operation,spend,blocker,seller))return false;
    const uint32 available=actor.GetPlayerbotAI()->GetAiObjectContext()->GetValue<uint32>("free money for",uint32(NeedMoneyFor::tradeskill))->Get();
    return WithinPurchaseBudget(spend,{policy.maxPurchasesPerHour,policy.maxDailySpendPercent},actor.GetMoney(),available,price,true,blocker);
}

bool LivingActivity::NativeAuctionOffers(Player& actor,uint32_t entry,uint32_t maximum,
    std::vector<NativeAuctionOffer>& offers,std::string& blocker,uint64_t auctioneer)
{
    offers.clear();const auto policy=GetOrganicAuctionPolicy();
    if(!sLivingActivityCoordinator.OnWorldThread() || !actor.GetPlayerbotAI() || !actor.GetSession() || !maximum ||
        policy.mode!="active" || !policy.buying) {blocker="profession_auction_purchasing_disabled";return false;}
    std::unique_lock<std::mutex> lock(sRandomPlayerbotMgr.m_ahActionMutex,std::try_to_lock);
    if(!lock.owns_lock()){blocker="profession_purchase_market_snapshot_busy";return false;}
    const auto* houseEntry=auctioneer ? actor.GetSession()->GetCheckedAuctionHouseForAuctioneer(ObjectGuid(auctioneer)) :
        sAuctionMgr.GetAuctionHouseEntry(&actor);
    auto* house=houseEntry?sAuctionMgr.GetAuctionsMap(houseEntry):nullptr;
    if(!house){blocker="profession_auction_unavailable";return false;}
    for(const auto& candidate:MaterialOffers(house,entry)) {
        const auto* offer=house->GetAuction(candidate.id);
        if(!offer || offer->itemTemplate!=entry || !offer->itemCount || offer->itemCount>maximum ||
            !offer->buyout || offer->buyout>actor.GetMoney() || offer->expireTime<=time(nullptr) ||
            offer->bidder==actor.GetGUIDLow() || !MaterialSeller(&actor,offer->owner))continue;
        offers.push_back({offer->Id,offer->itemTemplate,offer->itemCount,offer->buyout,offer->owner});
    }
    std::sort(offers.begin(),offers.end(),[](const auto& a,const auto& b) {
        const auto left=uint64_t(a.copper)*b.quantity,right=uint64_t(b.copper)*a.quantity;
        return left!=right ? left<right : a.id<b.id;
    });
    blocker=offers.empty()?"profession_auction_source_unavailable":"";return !offers.empty();
}

bool AhBidAction::HasPendingMaterial(Player* bot, uint32 entry)
{
    for (auto it = bot->GetMailBegin(); it != bot->GetMailEnd(); ++it)
    {
        const Mail* mail = *it;
        if (!mail || mail->state == MAIL_STATE_DELETED || mail->expire_time <= time(nullptr) || mail->COD) continue;
        for (const auto& item : mail->items)
            if (item.item_template == entry) return true;
    }
    return false; // Undelivered attachments count as pending, never as bag stock.
}

bool AhBidAction::HasMaterialOffer(Player* bot, uint32 entry, uint32 maximumCount)
{
    if (!bot || !maximumCount || !bot->GetSession() || !bot->GetPlayerbotAI()) return false;
    const auto policy = GetOrganicAuctionPolicy();
    if (policy.mode != "active" || !policy.buying || !policy.maxPurchasesPerHour) return false;
    std::unique_lock<std::mutex> lock(sRandomPlayerbotMgr.m_ahActionMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    const auto* houseEntry = sAuctionMgr.GetAuctionHouseEntry(bot);
    if (!houseEntry) return false;
    auto* house = sAuctionMgr.GetAuctionsMap(houseEntry);
    if (!house) return false;
    for (const auto& offer : MaterialOffers(house, entry))
        if (offer.count <= maximumCount && offer.price <= uint64(bot->GetMoney()) * policy.maxDailySpendPercent / 100 &&
            MaterialSeller(bot, offer.owner)) return true;
    return false;
}

bool AhBidAction::CollectRecipeMaterial(uint32 entry, std::string& blocker)
{
    if (!sLivingActivityCoordinator.PermitEffects(*ai, GetActivityEffects(), "native service mutation")) { blocker = "activity_authority_wait"; return false; }
    if (!bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->IsTaxiFlying() ||
        bot->GetTransport() || bot->IsBeingTeleported() || bot->GetMap()->IsDungeon())
    { blocker = "recipe_mail_collection_unsafe"; return false; }
    blocker = "recipe_materials_in_mail";
    for (auto it = bot->GetMailBegin(); it != bot->GetMailEnd(); ++it)
    {
        const Mail* mail = *it;
        if (!mail || mail->state == MAIL_STATE_DELETED || mail->expire_time <= time(nullptr) || mail->COD) continue;
        for (const auto& attachment : mail->items)
        {
            if (attachment.item_template != entry) continue;
            if (mail->deliver_time > time(nullptr)) { blocker = "recipe_mail_delivery_pending"; return false; }
            GameObject* mailbox = nullptr;
            for (const auto& guid : AI_VALUE(std::list<ObjectGuid>, "nearest game objects no los"))
            {
                auto* go = ai->GetGameObject(guid);
                if (go && go->GetGoType() == GAMEOBJECT_TYPE_MAILBOX && bot->IsWithinDistInMap(go, INTERACTION_DISTANCE))
                { mailbox = go; break; }
            }
            if (!mailbox) { blocker = "recipe_mailbox_out_of_range"; return false; }
            Item* item = bot->GetMItem(attachment.item_guid);
            if (!item) { blocker = "recipe_mail_attachment_unavailable"; return false; }
            const uint32 guid = attachment.item_guid, count = item->GetCount(), before = bot->GetItemCount(entry, false);
            const auto claims = sLivingActivityCoordinator.ResourceReservations().Inspect();
            if (!claims || claims->UnreservedItem(bot->GetGUIDLow(), guid, entry, count) != count)
            { blocker = "recipe_mail_owned_by_saved_task"; return false; }
            WorldPacket packet; packet << mailbox->GetObjectGuid() << mail->messageID;
#ifndef MANGOSBOT_ZERO
            packet << guid;
#endif
            bot->GetSession()->HandleMailTakeItem(packet);
            const bool received = !bot->GetMItem(guid) && bot->GetItemCount(entry, false) == before + count;
            PlayerbotServiceTracking::Result(bot, "profession_mail", mailbox->GetEntry(), entry,
                "received_items", 0, received ? count : 0);
            blocker = received ? "recipe_materials_received" : "recipe_mail_collection_rejected";
            return received;
        }
    }
    return false;
}

bool AhBidAction::BuyRecipeMaterial(uint32 entry, uint32 requiredCount, std::string& blocker)
{
    if (!sLivingActivityCoordinator.PermitEffects(*ai, GetActivityEffects(), "native service mutation")) { blocker = "activity_authority_wait"; return false; }
    blocker = "recipe_purchase_unsafe";
    if (!bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->IsTaxiFlying() ||
        bot->GetTransport() || bot->IsBeingTeleported() || bot->GetMap()->IsDungeon()) return false;
    const auto policy = GetOrganicAuctionPolicy();
    if (policy.mode != "active" || !policy.buying) { blocker = "recipe_purchasing_disabled"; return false; }
    const uint32 owned = bot->GetItemCount(entry, true);
    if (owned >= requiredCount) { blocker = "recipe_materials_already_owned"; return false; }
    if (HasPendingMaterial(bot, entry)) { blocker = "recipe_materials_in_mail"; return false; }
    if (sGuildSupplies.ReservedEntry(bot->GetGUIDLow(), entry) || ItemUsageValue::IsNeededForQuest(bot, entry, true))
    { blocker = "recipe_material_reserved"; return false; }
    Unit* auctioneer = nullptr;
    for (const auto& guid : AI_VALUE(std::list<ObjectGuid>, "nearest npcs"))
        if ((auctioneer = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_AUCTIONEER))) break;
    if (!auctioneer) { blocker = "recipe_auctioneer_out_of_range"; return false; }
    std::unique_lock<std::mutex> lock(sRandomPlayerbotMgr.m_ahActionMutex, std::try_to_lock);
    if (!lock.owns_lock()) { blocker = "recipe_auction_busy"; return false; }
    const auto* houseEntry = bot->GetSession()->GetCheckedAuctionHouseForAuctioneer(auctioneer->GetObjectGuid());
    auto* house = houseEntry ? sAuctionMgr.GetAuctionsMap(houseEntry) : nullptr;
    if (!house) { blocker = "recipe_auction_unavailable"; return false; }
    const auto spent=RecentSpending(bot->GetGUIDLow());
    if (!spent.complete) { blocker = "recipe_purchase_budget_unavailable"; return false; }
    if (spent.auctionCountHour >= policy.maxPurchasesPerHour)
    { blocker = "recipe_purchase_hourly_limit"; return false; }
    const uint32 budget = AI_VALUE2(uint32, "free money for", uint32(NeedMoneyFor::tradeskill));
    AuctionEntry* selected = nullptr;
    blocker = "recipe_no_affordable_exact_listing";
    for (const auto& offer : MaterialOffers(house, entry))
    {
        auto* auction = house->GetAuction(offer.id); // Never trust the cached listing at mutation time.
        if (!auction || auction->itemTemplate != entry || !auction->itemCount || !auction->buyout ||
            auction->expireTime <= time(nullptr) || auction->itemCount > requiredCount - owned ||
            auction->bidder == bot->GetGUIDLow() || !MaterialSeller(bot, auction->owner)) continue;
        const uint32 price = auction->buyout;
        const auto offerSpend=RecentSpending(bot->GetGUIDLow(),auction->owner);
        if (!AuctionPurchaseAllowed(offerSpend,policy,bot->GetMoney(),budget,price)) continue;
        if (!selected || uint64(price) * selected->itemCount < uint64(selected->buyout) * auction->itemCount)
            selected = auction;
    }
    if (!selected) return false;
    // Buyout is paid by the native handler and delivered through native mail.
    // No generic random bid and no synthetic item receipt.
    const bool bought = BidItem(nullptr, selected, selected->buyout, auctioneer, true, "recipe_material", true);
    blocker = bought ? "recipe_purchase_awaiting_mail" : "recipe_buyout_rejected";
    return bought;
}

bool AhAction::Execute(Event& event)
{
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    std::string text = event.getParam();

    std::list<ObjectGuid> npcs = AI_VALUE(std::list<ObjectGuid>, "nearest npcs");
    for (std::list<ObjectGuid>::iterator i = npcs.begin(); i != npcs.end(); i++)
    {
        Unit* npc = bot->GetNPCIfCanInteractWith(*i, UNIT_NPC_FLAG_AUCTIONEER);
        if (!npc)
            continue;

        if (!sRandomPlayerbotMgr.m_ahActionMutex.try_lock()) //Another bot is using the Auction right now. Try again later.
            return false;

        bool doneAuction = ExecuteCommand(requester, text, npc);

        sRandomPlayerbotMgr.m_ahActionMutex.unlock();

        return doneAuction;
    }

    ai->TellPlayerNoFacing(requester, "Cannot find auctioneer nearby");
    return false;
}

bool AhAction::ExecuteCommand(Player* requester, std::string text, Unit* auctioneer)
{
    uint32 time;
#ifdef MANGOSBOT_ZERO
    time = 8 * HOUR / MINUTE;
#else
    time = 12 * HOUR / MINUTE;
#endif

    if (text == "vendor")
    {
        OrganicAuctionPolicy policy = GetOrganicAuctionPolicy();
        if (policy.mode != "active" || !policy.posting)
            return false;
        uint32 listingLimit = ListingLimit(bot->GetLevel());
        uint32 activeListings = CharacterAuctionCount(bot->GetGUIDLow());
        if (activeListings >= listingLimit)
            return false;
        AuctionHouseEntry const* auctionHouseEntry = bot->GetSession()->GetCheckedAuctionHouseForAuctioneer(auctioneer->GetObjectGuid());
        if (!auctionHouseEntry)
            return false;

        std::list<Item*> items = AI_VALUE2(std::list<Item*>, "inventory items", "usage " + std::to_string((uint8)ItemUsage::ITEM_USAGE_AH));

        bool postedItem = false;

        std::map<uint32, uint32> pricePerItemCache;

        //resulting undercut value for reporting
        uint32 resultingUndercut = 0;
        uint32 postedItems = 0;

        for (auto item : items)
        {
            if (!LivingWowAuctionItemEligible(item))
                continue;
            if (activeListings + postedItems >= listingLimit)
                break;
            if (sPlayerbotActionBroker.IsItemReserved(item->GetGUIDLow()))
                continue;
            if (std::unique_ptr<QueryResult> acquired = CharacterDatabase.PQuery(
                "SELECT 1 FROM organic_economy_auction_history WHERE buyer_guid='%u' AND item_entry='%u' AND outcome='sold' AND occurred_at>DATE_SUB(NOW(),INTERVAL 1 DAY) LIMIT 1", bot->GetGUIDLow(), item->GetEntry()))
                continue;
            RESET_AI_VALUE2(ItemUsage, "item usage", ItemQualifier(item).GetQualifier());
            if(AI_VALUE2(ItemUsage, "item usage", ItemQualifier(item).GetQualifier()) != ItemUsage::ITEM_USAGE_AH)
                continue;

            auto pmo = sPerformanceMonitor.start(PERF_MON_VALUE, "IsMoreProfitableToSellToAHThanToVendor", ai);
            bool isMoreProfitableToSellToAHThanToVendor = ItemUsageValue::IsMoreProfitableToSellToAHThanToVendor(item->GetProto(), bot);
            pmo.reset();

            if (!isMoreProfitableToSellToAHThanToVendor)
                continue;

            uint32 deposit = AuctionHouseMgr::GetAuctionDeposit(auctionHouseEntry, time * MINUTE, item);

            RESET_AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::ah);
            uint32 freeMoney = AI_VALUE2(uint32, "free money for", (uint32)NeedMoneyFor::ah);

            if (deposit > freeMoney)
                return false;

            const ItemPrototype* proto = item->GetProto();

            if (!pricePerItemCache[proto->ItemId])
            {
                uint32 basePerItem = ItemUsageValue::GetBotSellPrice(proto, bot);
                uint32 initialPricePercentage = urand(75, 100);
                uint32 pricePerItem = (basePerItem * initialPricePercentage) / 100;
                if (!pricePerItem)
                    pricePerItem = 1;
                pricePerItemCache[proto->ItemId] = pricePerItem;
            }

            uint32 listingTime = time;
            if (proto->Quality >= ITEM_QUALITY_RARE)
                listingTime = 48 * 60;
            else if (proto->InventoryType != INVTYPE_NON_EQUIP)
                listingTime = 24 * 60;
            bool didPost = PostItem(requester, item, pricePerItemCache[proto->ItemId] * item->GetCount(), auctioneer, listingTime);

            if (didPost)
            {
                    postedItem |= true;
                    postedItems++;
            }

            if (!urand(0, 5 + (items.size()- postedItems)/10))
                break;
        }

        return postedItem;
    }

    int pos = text.find(" ");
    if (pos == std::string::npos) return false;

    std::string priceStr = text.substr(0, pos);
    uint32 price = ChatHelper::parseMoney(priceStr);

    std::list<Item*> found = ai->InventoryParseItems(text, IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
    if (found.empty())
        return false;

    Item* item = *found.begin();

    return PostItem(requester, item, price, auctioneer, time);
}

bool AhAction::PostItem(Player* requester, Item* item, uint32 price, Unit* auctioneer, uint32 time)
{
    // Recheck here for explicit commands and changes since candidate evaluation.
    // Rejected candidates are not attempted listings or completed operations.
    if (!LivingWowAuctionItemEligible(item)) return false;
    ObjectGuid itemGuid = item->GetObjectGuid();
    ItemPrototype const* proto = item->GetProto();

    ItemQualifier itemQualifier(item);

    uint32 cnt = item->GetCount();

    WorldPacket packet;
    packet << auctioneer->GetObjectGuid();
#ifdef MANGOSBOT_TWO
    packet << (uint32)1;
#endif
    packet << itemGuid;
#ifdef MANGOSBOT_TWO
    packet << cnt;
#endif
    packet << price * 95 / 100; //bid price?
    packet << price; //buyout price?
    packet << time;

    AuctionHouseEntry const* house = bot->GetSession()->GetCheckedAuctionHouseForAuctioneer(auctioneer->GetObjectGuid());
    const uint32 deposit = house ? AuctionHouseMgr::GetAuctionDeposit(house, time * MINUTE, item) : 0;
    bot->GetSession()->HandleAuctionSellItem(packet);
    uint32 postedAuction = 0;
    if (house)
        for (const auto& row : sAuctionMgr.GetAuctionsMap(house)->GetAuctions())
            if (row.second && row.second->owner == bot->GetGUIDLow() && row.second->itemGuidLow == itemGuid.GetCounter())
            { postedAuction = row.second->Id; break; }
    if (!PlayerbotServiceTracking::Result(bot, "auction_post", auctioneer->GetEntry(), proto->ItemId,
        "owned_auction_id", 0, postedAuction)) return false;

    if (bot->GetItemByGuid(itemGuid))
        return false;

    CharacterDatabase.PExecute("INSERT INTO organic_economy_auction_history "
        "(auction_id,auction_house_id,seller_guid,item_guid,item_entry,quantity,unit_price_copper,deposit_copper,outcome) "
        "VALUES (0,'%u','%u','%u','%u','%u','%u','%u','posted')",
        house ? house->houseId : 0, bot->GetGUIDLow(), itemGuid.GetCounter(), proto->ItemId, cnt, price / std::max<uint32>(1, cnt), deposit);
    sPlayerbotAIConfig.logEvent(ai, "AhAction", proto->Name1, std::to_string(proto->ItemId));

    std::ostringstream out;
    out << "Posting " << ChatHelper::formatItem(itemQualifier, cnt) << " for " << ChatHelper::formatMoney(price) << " to the AH";
    ai->TellPlayerNoFacing(requester, out.str(), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
    return true;
}

bool AhBidAction::ExecuteCommand(Player* requester, std::string text, Unit* auctioneer)
{
    OrganicAuctionPolicy policy = GetOrganicAuctionPolicy();
    if (text == "vendor" && (policy.mode != "active" || !policy.buying))
        return false;
    AuctionHouseEntry const* auctionHouseEntry = bot->GetSession()->GetCheckedAuctionHouseForAuctioneer(auctioneer->GetObjectGuid());
    if (!auctionHouseEntry)
        return false;

    // always return pointer
    AuctionHouseObject* auctionHouse = sAuctionMgr.GetAuctionsMap(auctionHouseEntry);

    if (!auctionHouse)
        return false;

    AuctionHouseObject::AuctionEntryMap const& map = auctionHouse->GetAuctions();

    if (map.empty())
        return false;

    AuctionEntry* auction = nullptr;

    std::vector<std::pair<AuctionEntry*, uint32>> auctionPowers;

    if (text == "vendor")
    {
        // Automatic self-use only. Explicit human item/budget commands below
        // retain their existing behavior. Index all three native houses once,
        // not a new per-bot timer or a database query for every candidate.
        std::set<uint32_t> recipeOrders;
        for (uint32_t house=0;house<MAX_AUCTION_HOUSE_TYPE;++house)
            for (const auto& row:sAuctionMgr.GetAuctionsMap(AuctionHouseType(house))->GetAuctions())
                if (row.second && row.second->bidder==bot->GetGUIDLow()) recipeOrders.insert(row.second->itemTemplate);
        auto recipeBlocked=[&](const AuctionEntry& listing) {
            const auto* proto=sObjectMgr.GetItemPrototype(listing.itemTemplate);
            if (!proto || proto->Class!=ITEM_CLASS_RECIPE) return false;
            return *LivingActivity::RecipePurchaseBlocker(
                LivingActivity::EvaluateUsefulRecipe(LivingActivity::InspectUsefulRecipe(*bot,proto)),
                LivingActivity::RecipeBookAlreadyOwnedOrIncoming(*bot,proto->ItemId),
                recipeOrders.count(proto->ItemId)!=0,listing.itemCount)!=0;
        };
        ItemUsage usage;
        auto data = WorldPacket();
        uint32 count, totalcount = 0;
        auctionHouse->BuildListBidderItems(data, bot, 9999, count, totalcount);

        if (totalcount > 10) //Already have 10 bids, stop.
            return false;

        std::unordered_map <ItemUsage, int32> freeMoney;

        freeMoney[ItemUsage::ITEM_USAGE_EQUIP] = freeMoney[ItemUsage::ITEM_USAGE_BAD_EQUIP] = (uint32)NeedMoneyFor::gear;
        freeMoney[ItemUsage::ITEM_USAGE_USE] = (uint32)NeedMoneyFor::consumables;
        freeMoney[ItemUsage::ITEM_USAGE_SKILL] = freeMoney[ItemUsage::ITEM_USAGE_DISENCHANT] =(uint32)NeedMoneyFor::tradeskill;
        freeMoney[ItemUsage::ITEM_USAGE_AMMO] = (uint32)NeedMoneyFor::ammo;
        freeMoney[ItemUsage::ITEM_USAGE_QUEST] = freeMoney[ItemUsage::ITEM_USAGE_AH] = freeMoney[ItemUsage::ITEM_USAGE_VENDOR] = freeMoney[ItemUsage::ITEM_USAGE_FORCE_NEED] = freeMoney[ItemUsage::ITEM_USAGE_FORCE_GREED] = (uint32)NeedMoneyFor::anything;

        uint32 checkNumAuctions = map.size();
        if (!sPlayerbotAIConfig.botCheckAllAuctionListings)
        {
            checkNumAuctions = urand(50, 250);
        }

        for (uint32 i = 0; i < checkNumAuctions; i++)
        {
            auto curAuction = std::next(std::begin(map), urand(0, map.size()-1));

            auction = curAuction->second;

            if (!auction)
                continue;

            if (std::find_if(auctionPowers.begin(), auctionPowers.end(), [auction](std::pair<AuctionEntry*, uint32> i){return i.first == auction;}) != auctionPowers.end())
                continue;

            auction = auctionHouse->GetAuction(auction->Id);

            if (!auction)
                continue;

            if (auction->owner == bot->GetGUIDLow() || auction->bidder == bot->GetGUIDLow())
                continue;
            if (recipeBlocked(*auction)) continue;
            // The exact recipe executor owns this demand, including mail waits.
            // Generic useful-item buying must not issue a competing order.
            if (sPlayerbotOrganicEconomy.RecipeMaterialQuantity(bot->GetGUIDLow(), auction->itemTemplate)) continue;
            uint32 sellerAccount = CharacterAccount(auction->owner);
            if (!sellerAccount || sellerAccount == bot->GetSession()->GetAccountId())
                continue;
            const auto spentToday=RecentSpending(bot->GetGUIDLow(),auction->owner);
            if (!spentToday.complete || spentToday.auctionCountHour >= policy.maxPurchasesPerHour)
                break;
            if (spentToday.sellerPurchasesWeek >= 3)
                continue;

            uint32 totalCost = LivingAuctionMinimumBid(auction->startbid, auction->bid, auction->GetAuctionOutBid(), auction->buyout);

            if (!totalCost) continue;
            usage = AI_VALUE2(ItemUsage, "item usage", ItemQualifier(auction).GetQualifier());

            if (freeMoney.find(usage) == freeMoney.end() || totalCost > AI_VALUE2(uint32, "free money for", freeMoney[usage]))
                continue;

            uint32 power = 1;

            switch (usage)
            {
            case ItemUsage::ITEM_USAGE_EQUIP:
            case ItemUsage::ITEM_USAGE_BAD_EQUIP:
                power = sRandomItemMgr.GetLiveStatWeight(bot, auction->itemTemplate);
                break;
            case ItemUsage::ITEM_USAGE_AH:
            {
                // Organic buyers must have a real use; pure bot arbitrage creates churn.
                continue;
                auto pmo = sPerformanceMonitor.start(PERF_MON_VALUE, "IsWorthBuyingFromAhToResellAtAH", ai);
                bool isWorthBuyingFromAhToResellAtAH = ItemUsageValue::IsWorthBuyingFromAhToResellAtAH(sObjectMgr.GetItemPrototype(auction->itemTemplate), totalCost, auction->itemCount);
                pmo.reset();

                if (!isWorthBuyingFromAhToResellAtAH)
                    continue;
                power = 1000;
                break;
            }
            case ItemUsage::ITEM_USAGE_VENDOR:
                //basically if AH price is lower than vendor sell price then it's worth it
                if (totalCost / auction->itemCount >= (int32)sObjectMgr.GetItemPrototype(auction->itemTemplate)->SellPrice)
                    continue;
                power = 1000;
                break;
            case ItemUsage::ITEM_USAGE_FORCE_NEED:
            case ItemUsage::ITEM_USAGE_FORCE_GREED:
                power = 1000;
                break;
            }

            power *= 1000;
            power /= (totalCost +1);
            if (!sPlayerbotAIConfig.IsInRandomAccountList(sellerAccount))
                power = uint32(double(power) * (1.0 + double(policy.humanPreference) / 100.0));
            if (!AuctionPurchaseAllowed(spentToday,policy,bot->GetMoney(),bot->GetMoney(),totalCost)) continue;

            auctionPowers.push_back(std::make_pair(auction, power));
        }

        std::sort(auctionPowers.begin(), auctionPowers.end(), [](std::pair<AuctionEntry*, uint32> i, std::pair<AuctionEntry*, uint32> j) {return i > j; });

        bool bidItems = false;

        for (auto auctionPower : auctionPowers)
        {
            auction = auctionPower.first;

            if (!auction)
                continue;

            auction = auctionHouse->GetAuction(auction->Id);

            if (!auction)
                continue;

            usage = AI_VALUE2(ItemUsage, "item usage", ItemQualifier(auction).GetQualifier());

            uint32 currentBidPrice = LivingAuctionMinimumBid(auction->startbid, auction->bid, auction->GetAuctionOutBid(), auction->buyout);
            if (!currentBidPrice) continue;
            uint32 currentBuyoutPrice = auction->buyout;

            bool shouldBuyout = false;

            //determine if should look at buyout or bid price depending on item usage
            uint32 price = currentBuyoutPrice ? currentBuyoutPrice : currentBidPrice;

            if (usage == ItemUsage::ITEM_USAGE_VENDOR || usage == ItemUsage::ITEM_USAGE_FORCE_GREED || usage == ItemUsage::ITEM_USAGE_NONE)
            {
                //do not care for buyout price for items that bot does not need
                price = currentBidPrice;
            }
            else if (currentBidPrice < static_cast<uint32>(currentBuyoutPrice * 0.3f) && !urand(0,1))
            {
                //if bid price < 30% of buyout, then might as well (50/50) chance consider bid price directly
                price = currentBidPrice;
            }

            //first check if has money for buyout price (if checking against buyout price)
            if (price == currentBuyoutPrice && (freeMoney.find(usage) == freeMoney.end() || price > AI_VALUE2(uint32, "free money for", freeMoney[usage])))
            {
                //check for free money for bid price next if has no money for buyout
                price = currentBidPrice;
            }

            //check if have money for bid price (if checking against bid price)
            if (price != currentBuyoutPrice && (freeMoney.find(usage) == freeMoney.end() || price > AI_VALUE2(uint32, "free money for", freeMoney[usage])))
            {
                if (!urand(0, 5))
                    break;
                else
                    continue;
            }

            freeMoney[ItemUsage::ITEM_USAGE_EQUIP] = freeMoney[ItemUsage::ITEM_USAGE_BAD_EQUIP] = (uint32)NeedMoneyFor::gear;
            freeMoney[ItemUsage::ITEM_USAGE_USE] = (uint32)NeedMoneyFor::consumables;
            freeMoney[ItemUsage::ITEM_USAGE_SKILL] = freeMoney[ItemUsage::ITEM_USAGE_DISENCHANT] = (uint32)NeedMoneyFor::tradeskill;
            freeMoney[ItemUsage::ITEM_USAGE_AMMO] = (uint32)NeedMoneyFor::ammo;
            freeMoney[ItemUsage::ITEM_USAGE_QUEST] = freeMoney[ItemUsage::ITEM_USAGE_AH] = freeMoney[ItemUsage::ITEM_USAGE_VENDOR] = freeMoney[ItemUsage::ITEM_USAGE_FORCE_NEED] = freeMoney[ItemUsage::ITEM_USAGE_FORCE_GREED] = (uint32)NeedMoneyFor::anything;
         
            ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", ItemQualifier(auction).GetQualifier());

            std::string reason = ItemUsageValue::ReasonForNeed(usage, auction, auction->itemCount, bot);            

            // Recheck the actual chosen price: the candidate may have been ranked
            // by a smaller bid, or earlier purchases may have consumed the budget.
            const auto spentToday=RecentSpending(bot->GetGUIDLow(),auction->owner);
            if (!spentToday.complete || spentToday.auctionCountHour >= policy.maxPurchasesPerHour) break;
            if (!price || price > bot->GetMoney() || freeMoney.find(usage) == freeMoney.end() ||
                price > AI_VALUE2(uint32, "free money for", freeMoney[usage]) ||
                !AuctionPurchaseAllowed(spentToday,policy,bot->GetMoney(),bot->GetMoney(),price))
                continue;
            if (recipeBlocked(*auction)) continue;
            const uint32_t orderedEntry=auction->itemTemplate; // Buyout destroys the native listing.
            const bool placedBid = BidItem(requester, auction, price, auctioneer, currentBuyoutPrice && price == currentBuyoutPrice, reason);
            bidItems = placedBid || bidItems;

            if (placedBid) {
                totalcount++;
                recipeOrders.insert(orderedEntry);
            }

            if (!urand(0, 5) || totalcount > 10)
                break;

            RESET_AI_VALUE2(uint32, "free money for", freeMoney[usage]);
        }

        return bidItems;
    }

    int pos = text.find(" ");
    if (pos == std::string::npos) return false;

    std::string priceStr = text.substr(0, pos);
    uint32 price = ChatHelper::parseMoney(priceStr);

    for (auto curAuction : map)
    {
        auction = curAuction.second;

        if (auction->owner == bot->GetGUIDLow() || auction->bidder == bot->GetGUIDLow())
            continue;

        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(auction->itemTemplate);

        if (!proto)
            continue;

        if(!proto->Name1)
            continue;

        if (!strstri(proto->Name1, text.c_str()))
            continue;

        uint32 cost = LivingAuctionMinimumBid(auction->startbid, auction->bid, auction->GetAuctionOutBid(), auction->buyout);

        if (!cost || (price && cost > price)) continue;

        uint32 power = auction->itemCount;
        power *= 1000;
        power /= cost;

        auctionPowers.push_back(std::make_pair(auction, power));
    }

    if (auctionPowers.empty())
        return false;

    std::sort(auctionPowers.begin(), auctionPowers.end(), [](std::pair<AuctionEntry*, uint32> i, std::pair<AuctionEntry*, uint32> j) {return i > j; });

    auction = auctionPowers.begin()->first;

    uint32 cost = LivingAuctionMinimumBid(auction->startbid, auction->bid, auction->GetAuctionOutBid(), auction->buyout);

    return BidItem(requester, auction, cost, auctioneer, cost == auction->buyout);
}

bool AhBidAction::BidItem(Player* requester, AuctionEntry* auction, uint32 price, Unit* auctioneer, bool isBuyout, std::string reason, bool quiet)
{
    if (!sLivingActivityCoordinator.PermitEffects(*ai, GetActivityEffects(), "native service mutation")) return false;
    AuctionHouseEntry const* auctionHouseEntry = bot->GetSession()->GetCheckedAuctionHouseForAuctioneer(auctioneer->GetObjectGuid());
    if (!auctionHouseEntry)
        return false;

    // always return pointer
    AuctionHouseObject* auctionHouse = sAuctionMgr.GetAuctionsMap(auctionHouseEntry);

    if (!auctionHouse)
        return false;

    auction = auctionHouse->GetAuction(auction->Id);

    if (!auction)
        return false;

    WorldPacket packet;
    packet << auctioneer->GetObjectGuid();
    packet << auction->Id;
    packet << price;

    uint32 oldMoney = bot->GetMoney();
    ItemQualifier itemQualifier(auction);
    uint32 count = auction->itemCount;

    ItemPrototype const* proto = sObjectMgr.GetItemPrototype(auction->itemTemplate);

    // A successful buyout deletes AuctionEntry inside the core handler.
    const uint32 auctionId = auction->Id;
    const uint32 sellerGuid = auction->owner;
    const uint32 itemGuid = auction->itemGuidLow;
    const uint32 auctionItemEntry = auction->itemTemplate;
    // Mark the whole debit/history-enqueue interval, not just the final wallet
    // change, so a concurrent asynchronous budget read cannot reuse old funds.
    LivingActivity::PurchaseMutation purchaseMutation(bot->GetGUIDLow());
    bot->GetSession()->HandleAuctionPlaceBid(packet);
    PlayerbotServiceTracking::Result(bot, "auction_bid", auctioneer->GetEntry(), auctionItemEntry,
        "money_debited_for_bid", oldMoney, bot->GetMoney(), false);

    if (bot->GetMoney() < oldMoney)
    {
        CharacterDatabase.PExecute("INSERT INTO organic_economy_auction_history "
            "(auction_id,auction_house_id,seller_guid,buyer_guid,item_guid,item_entry,quantity,unit_price_copper,outcome) "
            "VALUES ('%u','%u','%u','%u','%u','%u','%u','%u','%s')", auctionId, auctionHouseEntry->houseId,
            sellerGuid, bot->GetGUIDLow(), itemGuid, auctionItemEntry, count,
            price / std::max<uint32>(1, count) + (price % std::max<uint32>(1, count) != 0), isBuyout ? "sold" : "bid");
        sPlayerbotAIConfig.logEvent(ai, "AhBidAction", proto->Name1, std::to_string(proto->ItemId));
        std::ostringstream out;
        if (isBuyout)
        {
            out << "Buying out " << ChatHelper::formatItem(itemQualifier, count) << " for " << ChatHelper::formatMoney(price) << " on the AH";
        }
        else
        {
            out << "Bidding " << ChatHelper::formatMoney(price) << " on " << ChatHelper::formatItem(itemQualifier, count) << " on the AH";
        }
        if (!reason.empty())
            out << " " << reason;
        if (!quiet) ai->TellPlayerNoFacing(requester, out.str(), PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
        return true;
    }
    return false;
}
