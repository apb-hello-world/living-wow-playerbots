#include "LivingGuildDeposit.h"
namespace LivingActivity {
std::string EncodeGuildDepositQuote(const GuildDepositQuote& q) {
    return "{\"job\":"+EncodeGuildDeliveryJob(q.job)+",\"actor\":"+std::to_string(q.actor)+
        ",\"item\":"+std::to_string(q.item)+",\"item_count\":"+std::to_string(q.itemCount)+
        ",\"amount\":"+std::to_string(q.amount)+",\"deposited\":"+std::to_string(q.deposited)+
        ",\"bank_count\":"+std::to_string(q.bankCount)+",\"bag_count\":"+std::to_string(q.bagCount)+
        ",\"money\":"+std::to_string(q.money)+",\"goal_target\":"+std::to_string(q.goalTarget)+
        ",\"goal_reserved\":"+std::to_string(q.goalReserved)+
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
        q.goalReserved=p.get<uint32_t>("goal_reserved");
        q.position=p.get<uint16_t>("position");const auto tab=p.get<uint32_t>("tab");if(tab>=6)return false;
        q.tab=uint8_t(tab);q.bank=p.get<uint64_t>("bank");
        if(!ValidGuildDepositQuote(q) || EncodeGuildDepositQuote(q)!=text)return false;
        quote=q;return true;
    } catch(const std::exception&) {return false;}
}
std::string GuildDepositNativeProof(const GuildDepositQuote& q,const Task& outcome,
    uint32_t sourceAfter,uint32_t bagsAfter,uint32_t bankAfter,uint32_t moneyAfter) {
    const bool moved=VerifyGuildDeposit(q,sourceAfter,bagsAfter,bankAfter,moneyAfter);
    const bool unchanged=sourceAfter==q.itemCount && bagsAfter==q.bagCount && bankAfter==q.bankCount && moneyAfter==q.money;
    if(!ValidGuildDepositQuote(q) || !IsUuid(outcome.id) || !outcome.revision || outcome.actor!=q.actor ||
        (!moved && !unchanged))return {};
    std::string proof="SELECT "+SqlValue(outcome.id)+','+std::to_string(outcome.revision)+
        " FROM characters c JOIN guild_society_supply_delivery d ON d.carrier_guid=c.guid WHERE c.guid="+std::to_string(q.actor)+
        " AND c.money="+std::to_string(q.money)+" AND d.delivery_id="+std::to_string(q.job.delivery)+
        " AND d.guild_id="+std::to_string(q.job.guild)+" AND d.goal_id="+SqlValue(q.job.goal)+
        " AND d.donor_guid="+std::to_string(q.job.donor)+" AND d.item_entry="+std::to_string(q.job.entry)+
        " AND d.quantity="+std::to_string(q.job.quantity)+" AND d.mail_id="+std::to_string(q.job.incomingMail)+
        " AND d.deposited_quantity="+std::to_string(q.deposited+(moved?q.amount:0));
    // Cancellation/revocation can correctly reject an already journaled intent.
    // Prove that rejection had NO effect; do not require the withdrawn permission.
    if(moved)proof+=" AND d.phase="+SqlValue(q.deposited+q.amount==q.job.quantity?"completed":"carried")+
        " AND EXISTS(SELECT 1 FROM guild_society_supply_goal g JOIN guild_society_supply_execution e ON e.guild_id=g.guild_id"
        " WHERE g.guild_id=d.guild_id AND g.goal_id=d.goal_id AND g.state='active' AND g.request_kind='item'"
        " AND g.item_entry="+std::to_string(q.job.entry)+" AND g.required_quantity="+std::to_string(q.goalTarget)+
        " AND g.reserved_quantity="+std::to_string(q.goalReserved)+" AND e.enabled=1)";
    proof+=" AND (SELECT COALESCE(SUM(i.count),0) FROM guild_bank_item b JOIN item_instance i ON i.guid=b.item_guid"
        " WHERE b.guildid="+std::to_string(q.job.guild)+" AND b.item_entry="+std::to_string(q.job.entry)+")="+std::to_string(bankAfter);
    if(sourceAfter)proof+=" AND EXISTS(SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid="+
        std::to_string(q.actor)+" AND v.item="+std::to_string(q.item)+" AND i.itemEntry="+std::to_string(q.job.entry)+
        " AND i.count="+std::to_string(sourceAfter)+" AND i.owner_guid="+std::to_string(q.actor)+')';
    else proof+=" AND NOT EXISTS(SELECT 1 FROM character_inventory WHERE item="+std::to_string(q.item)+')';
    return proof;
}
}
