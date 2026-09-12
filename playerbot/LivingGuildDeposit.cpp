#include "LivingGuildDeposit.h"
namespace LivingActivity {
std::string EncodeGuildDepositQuote(const GuildDepositQuote& q) {
    return "{\"job\":"+EncodeGuildDeliveryJob(q.job)+",\"actor\":"+std::to_string(q.actor)+
        ",\"item\":"+std::to_string(q.item)+",\"item_count\":"+std::to_string(q.itemCount)+
        ",\"amount\":"+std::to_string(q.amount)+",\"deposited\":"+std::to_string(q.deposited)+
        ",\"bank_count\":"+std::to_string(q.bankCount)+",\"bag_count\":"+std::to_string(q.bagCount)+
        ",\"money\":"+std::to_string(q.money)+",\"goal_target\":"+std::to_string(q.goalTarget)+
        ",\"goal_reserved\":"+std::to_string(q.goalReserved)+",\"goal_updated\":"+std::to_string(q.goalUpdated)+
        ",\"position\":"+std::to_string(q.position)+",\"tab\":"+std::to_string(q.tab)+
        ",\"bank\":"+std::to_string(q.bank)+'}';
}
bool DecodeGuildDepositQuote(const std::string& text,GuildDepositQuote& quote) {
    quote={};if(text.size()>3072)return false;
    try {
        boost::property_tree::ptree p;std::istringstream input(text);boost::property_tree::read_json(input,p);
        std::ostringstream job;boost::property_tree::write_json(job,p.get_child("job"),false);
        GuildDepositQuote q;std::string blocker;
        if(!DecodeGuildDeliveryJob(job.str(),q.job,blocker))return false;
        q.actor=p.get<uint32_t>("actor");q.item=p.get<uint32_t>("item");q.itemCount=p.get<uint32_t>("item_count");
        q.amount=p.get<uint32_t>("amount");q.deposited=p.get<uint32_t>("deposited");q.bankCount=p.get<uint32_t>("bank_count");
        q.bagCount=p.get<uint32_t>("bag_count");q.money=p.get<uint32_t>("money");q.goalTarget=p.get<uint32_t>("goal_target");
        q.goalReserved=p.get<uint32_t>("goal_reserved");q.goalUpdated=p.get<uint32_t>("goal_updated");
        q.position=p.get<uint16_t>("position");const auto tab=p.get<uint32_t>("tab");if(tab>=6)return false;
        q.tab=uint8_t(tab);q.bank=p.get<uint64_t>("bank");
        if(!ValidGuildDepositQuote(q) || EncodeGuildDepositQuote(q)!=text)return false;
        quote=q;return true;
    } catch(const std::exception&) {return false;}
}
}
