#ifndef _PLAYERBOT_ACTION_BROKER_H
#define _PLAYERBOT_ACTION_BROKER_H

#include <chrono>
#include <map>
#include <sstream>
#include <string>
#include <vector>

class Player;

struct ChatDirectorEvent;
struct ChatDirectorCandidate;
namespace LivingWowChatJson { struct EconomicQuote; }

struct ChatDirectorActionProposal
{
    std::string proposalId;
    uint32 botGuid = 0;
    uint32 targetGuid = 0;
    std::string type;
    std::string capabilityRef;
    uint32 quantity = 0;
    uint32 priceCopper = 0;
    std::string delivery;
    std::string intent;
    std::string quoteId;
};

struct PlayerbotActionResult
{
    PlayerbotActionResult(bool createdValue = false, const std::string& reason = "",
        const std::string& message = "") : created(createdValue), reasonCode(reason), playerMessage(message) {}

    bool created;
    std::string reasonCode;
    std::string playerMessage;
};

class PlayerbotActionBroker
{
public:
    static PlayerbotActionBroker& instance();
    PlayerbotActionResult Create(const ChatDirectorActionProposal& proposal, const ChatDirectorEvent& event);
    bool Authorizes(Player* bot, Player* trader) const;
    bool IsItemReserved(uint32 itemGuid) const;
    uint32 ReservedCopper(uint32 botGuid) const;
    bool PopulateTrade(Player* bot, Player* trader);
    bool ValidateTrade(Player* bot, Player* trader);
    void CompleteTrade(Player* bot, Player* trader);
    void CancelTrade(Player* bot, Player* trader, const std::string& reason);
    void CancelTrade(Player* bot, const std::string& reason);
    void Update();
    void ReportRejected(const ChatDirectorActionProposal& proposal, const ChatDirectorEvent& event,
        const std::string& reason) const;
    void UpsertEconomicQuote(const LivingWowChatJson::EconomicQuote& quote, bool update);
    void AppendEconomicQuotesJson(uint32 playerGuid, const std::map<uint32, ChatDirectorCandidate>& candidates,
        std::ostringstream& json) const;

private:
    struct EconomicQuoteState
    {
        std::string quoteId, state, direction, capabilityRef, delivery, freeGiftDisposition;
        uint32 botGuid = 0, playerGuid = 0, itemEntry = 0, quantity = 0;
        uint32 openingPrice = 0, currentPrice = 0, limitPrice = 0, rounds = 0, maximumRounds = 0;
        std::chrono::steady_clock::time_point expires;
    };

    struct Transaction
    {
        std::string transactionId;
        std::string commissionId;
        std::string eventId;
        std::string proposalId;
        uint32 botGuid = 0;
        uint32 playerGuid = 0;
        uint32 itemEntry = 0;
        uint32 itemGuid = 0;
        uint32 spellId = 0;
        uint32 quantity = 0;
        uint32 priceCopper = 0;
        std::string type;
        std::string delivery;
        std::string state;
        std::string failureReason;
        std::chrono::steady_clock::time_point expires;
        std::chrono::steady_clock::time_point preparingSince;
        std::chrono::steady_clock::time_point lastMeetingMove;
        std::chrono::steady_clock::time_point lastTradeAttempt;
        uint32 tradeOpenAttempts = 0;
    };

    Transaction* Find(uint32 botGuid, uint32 playerGuid);
    const Transaction* Find(uint32 botGuid, uint32 playerGuid) const;
    void Report(const Transaction& transaction) const;
    std::map<std::string, Transaction> transactions;
    std::map<uint32, std::string> reservedItems;
    std::map<uint32, uint32> reservedMoney;
    std::map<uint32, std::vector<std::chrono::steady_clock::time_point>> giftHistory;
    std::map<std::string, EconomicQuoteState> economicQuotes;
};

#define sPlayerbotActionBroker PlayerbotActionBroker::instance()

#endif
