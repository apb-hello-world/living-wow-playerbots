#include "LivingQuestReward.h"
#include <cassert>
#include <limits>
using namespace LivingActivity;
int main() {
    QuestRewardQuote q;q.actor=658;q.quest=47;q.giver=123;q.level=11;q.xp=50;q.money=100;q.moneyDelta=175;
    q.items={{773,12,10,0},{1191,2,0,1}};
    assert(ValidQuestRewardQuote(q));QuestRewardQuote parsed;
    assert(DecodeQuestRewardQuote(EncodeQuestRewardQuote(q),parsed));
    assert(EncodeQuestRewardQuote(parsed)==EncodeQuestRewardQuote(q));
    assert(QuestRewardCountsMatch(q,275,{2,3}));
    assert(!QuestRewardCountsMatch(q,275,{12,3})); // No reward without real consumption.
    assert(!QuestRewardCountsMatch(q,275,{2,2}));
    assert(!QuestRewardCountsMatch(q,100,{2,3}));
    q.moneyDelta=-100;assert(QuestRewardCountsMatch(q,0,{2,3}));
    q.moneyDelta=-101;assert(!ValidQuestRewardQuote(q));q.moneyDelta=0;
    q.items.push_back(q.items.front());assert(!ValidQuestRewardQuote(q));q.items.pop_back();
    q.items[0].consume=13;assert(!ValidQuestRewardQuote(q));q.items[0].consume=10;
    q.items[1].before=std::numeric_limits<uint32_t>::max();assert(!ValidQuestRewardQuote(q));q.items[1].before=2;
    q.choice=6;assert(!ValidQuestRewardQuote(q));q.choice=0;
    assert(!DecodeQuestRewardQuote("{}",parsed));assert(!DecodeQuestRewardQuote(std::string(4097,'x'),parsed));
    q.items={{773,12,10,2}};assert(QuestRewardCountsMatch(q,100,{4})); // Same-entry reward aggregated once.
    q.items.clear();assert(QuestRewardCountsMatch(q,100,{})); // Kill-only quest, no fabricated resource claim.
}
