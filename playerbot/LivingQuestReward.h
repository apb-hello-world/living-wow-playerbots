#pragma once
#include "LivingActivity.h"
#include <vector>
namespace LivingActivity {
struct QuestRewardItem {
    uint32_t entry=0,before=0,consume=0,gain=0;
};
struct QuestRewardQuote {
    uint32_t actor=0,quest=0,choice=0,money=0,level=0,xp=0;
    uint64_t giver=0;
    int64_t moneyDelta=0;
    std::vector<QuestRewardItem> items;
};
bool ValidQuestRewardQuote(const QuestRewardQuote&);
std::string EncodeQuestRewardQuote(const QuestRewardQuote&);
bool DecodeQuestRewardQuote(const std::string&,QuestRewardQuote&);
bool QuestRewardCountsMatch(const QuestRewardQuote&,uint32_t money,const std::vector<uint32_t>& counts);
}
