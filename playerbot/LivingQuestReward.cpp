#include "LivingQuestReward.h"
#include <boost/property_tree/json_parser.hpp>
#include <limits>
#include <set>
#include <sstream>
namespace LivingActivity {
bool ValidQuestRewardQuote(const QuestRewardQuote& q) {
    if(!q.actor || !q.quest || !q.giver || q.choice>=6 || !q.level || q.level>70 || q.items.size()>16 ||
        q.moneyDelta<-int64_t(q.money) || q.moneyDelta>int64_t(std::numeric_limits<uint32_t>::max())-q.money)return false;
    std::set<uint32_t> entries;
    for(const auto& item:q.items)if(!item.entry || !entries.insert(item.entry).second ||
        item.consume>item.before || (!item.consume && !item.gain) ||
        uint64_t(item.before)-item.consume+item.gain>std::numeric_limits<uint32_t>::max())return false;
    return true;
}
std::string EncodeQuestRewardQuote(const QuestRewardQuote& q) {
    std::string out="{\"actor\":"+std::to_string(q.actor)+",\"quest\":"+std::to_string(q.quest)+
        ",\"giver\":"+std::to_string(q.giver)+",\"choice\":"+std::to_string(q.choice)+
        ",\"money\":"+std::to_string(q.money)+",\"level\":"+std::to_string(q.level)+
        ",\"xp\":"+std::to_string(q.xp)+",\"money_delta\":"+std::to_string(q.moneyDelta)+",\"items\":[";
    bool first=true;
    for(const auto& i:q.items) {
        if(!first)out+=',';
        first=false;
        out+="{\"entry\":"+std::to_string(i.entry)+",\"before\":"+std::to_string(i.before)+
            ",\"consume\":"+std::to_string(i.consume)+",\"gain\":"+std::to_string(i.gain)+'}';
    }
    return out+"]}";
}
bool DecodeQuestRewardQuote(const std::string& text,QuestRewardQuote& q) {
    q={};if(text.size()>4096)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(text);boost::property_tree::read_json(in,p);
        if(p.size()!=9)return false;
        q.actor=p.get<uint32_t>("actor");q.quest=p.get<uint32_t>("quest");q.giver=p.get<uint64_t>("giver");
        q.choice=p.get<uint32_t>("choice");q.money=p.get<uint32_t>("money");q.level=p.get<uint32_t>("level");
        q.xp=p.get<uint32_t>("xp");q.moneyDelta=p.get<int64_t>("money_delta");
        for(const auto& row:p.get_child("items")) {
            if(!row.first.empty() || row.second.size()!=4 || q.items.size()>=16)return false;
            const auto& i=row.second;q.items.push_back({i.get<uint32_t>("entry"),i.get<uint32_t>("before"),
                i.get<uint32_t>("consume"),i.get<uint32_t>("gain")});
        }
        return ValidQuestRewardQuote(q);
    } catch(const std::exception&) {q={};return false;}
}
bool QuestRewardCountsMatch(const QuestRewardQuote& q,uint32_t money,const std::vector<uint32_t>& counts) {
    if(!ValidQuestRewardQuote(q) || int64_t(money)!=int64_t(q.money)+q.moneyDelta || counts.size()!=q.items.size())return false;
    for(size_t n=0;n<counts.size();++n)if(counts[n]!=q.items[n].before-q.items[n].consume+q.items[n].gain)return false;
    return true;
}
}
