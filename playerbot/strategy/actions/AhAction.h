#pragma once
#include "GenericActions.h"

namespace ai
{
    class AhAction : public ChatCommandAction
    {
    public:
        AhAction(PlayerbotAI* ai, std::string name = "ah") : ChatCommandAction(ai, name) {}
        LivingActivity::Effects GetActivityEffects() const override {
            using namespace LivingActivity;
            return {Mask(Effect::Inventory) | Mask(Effect::Money) | Mask(Effect::Social), Lane::Managed, true};
        }
        virtual bool Execute(Event& event) override;
        // Read-only access to the existing organic posting policy. Managed
        // services use the same limits/classification, not a second economy.
        static bool PostingCapacity(Player&, uint32& available, std::string& blocker);
        static bool PostingItem(Player&, Item&, uint32& unitPrice, uint32& minutes, std::string& blocker);
        static uint32 PostingMoney(Player&);

    private:
        virtual bool ExecuteCommand(Player* requester, std::string text, Unit* auctioneer);
        bool PostItem(Player* requester, Item* item, uint32 price, Unit* auctioneer, uint32 time);       

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "ah"; } //Must equal iternal name
        virtual std::string GetHelpDescription()
        {
            return "This command will make bots auction items to a nearby auction houses.\n"
                "Usage: ah [itemlink] <money>\n"
                "Example: ah vendor (post items based on item use)\n"
                "Example: ah [itemlink] 5g\n";
        }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return { "nearest npcs", "inventory items", "item usage", "free money for" }; }
#endif
    };

    class AhBidAction : public AhAction
    {
    public:
        AhBidAction(PlayerbotAI* ai) : AhAction(ai, "ah bid") {}
        static bool HasMaterialOffer(Player* bot, uint32 entry, uint32 maximumCount);
        static bool HasPendingMaterial(Player* bot, uint32 entry);
        bool CollectRecipeMaterial(uint32 entry, std::string& blocker);
        bool BuyRecipeMaterial(uint32 entry, uint32 requiredCount, std::string& blocker);

#ifdef GenerateBotHelp
        virtual std::string GetHelpName() { return "ah bid"; } //Must equal iternal name
        virtual std::string GetHelpDescription()
        {
            return "This command will make bots bid on a specific item with a specific budget on a nearby auctionhouse.\n"
                "The highest item/gold auction will be used that falls below the given budget.\n"
                "Usage: ah bid [itemlink] <money>\n"
                "Example: ah bid vendor (bid on items based on item use)\n"
                "Example: ah bid [itemlink] 5g\n";
        }
        virtual std::vector<std::string> GetUsedActions() { return {}; }
        virtual std::vector<std::string> GetUsedValues() { return { "nearest npcs", "item usage", "free money for" }; }
#endif 
    private:
        virtual bool ExecuteCommand(Player* requester, std::string text, Unit* auctioneer);
        bool BidItem(Player* requester, AuctionEntry* auction, uint32 price, Unit* auctioneer, bool isBuyout, std::string reason = "", bool quiet = false);
    };
}
