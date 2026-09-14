#include "LivingCommissionMail.h"

namespace LivingActivity {
bool ValidCommissionMailQuote(const CommissionMailQuote& q) {
    return q.commission.size()>=5 && q.commission.size()<=36 && q.commission.compare(0,4,"lwc-")==0 &&
        q.commission.find_first_not_of("0123456789",4)==std::string::npos && q.sender && q.receiver &&
        q.sender!=q.receiver && q.item && q.entry && q.quantity==1 && q.count==q.quantity &&
        q.postage==30 && q.moneyBefore>=q.postage && q.mailbox && q.delay<=30u*86400u;
}
std::string EncodeCommissionMailQuote(const CommissionMailQuote& q) {
    if(!ValidCommissionMailQuote(q))throw std::invalid_argument("exact_commission_mail_quote_required");
    boost::property_tree::ptree p;p.put("version",1);p.put("commission",q.commission);
    p.put("sender",q.sender);p.put("receiver",q.receiver);p.put("item",q.item);p.put("entry",q.entry);
    p.put("quantity",q.quantity);p.put("count",q.count);p.put("money_before",q.moneyBefore);
    p.put("postage",q.postage);p.put("cod",q.cod);p.put("delay",q.delay);p.put("position",q.position);p.put("mailbox",q.mailbox);
    std::ostringstream out;boost::property_tree::write_json(out,p,false);return out.str();
}
bool DecodeCommissionMailQuote(const std::string& text,CommissionMailQuote& out) {
    out={};if(text.empty() || text.size()>2048)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(text);boost::property_tree::read_json(in,p);
        CommissionMailQuote q;q.commission=p.get<std::string>("commission");
        q.sender=p.get<uint32_t>("sender");q.receiver=p.get<uint32_t>("receiver");q.item=p.get<uint32_t>("item");
        q.entry=p.get<uint32_t>("entry");q.quantity=p.get<uint32_t>("quantity");q.count=p.get<uint32_t>("count");
        q.moneyBefore=p.get<uint32_t>("money_before");q.postage=p.get<uint32_t>("postage");q.cod=p.get<uint32_t>("cod");
        q.delay=p.get<uint32_t>("delay");q.position=p.get<uint16_t>("position");q.mailbox=p.get<uint64_t>("mailbox");
        if(!ValidCommissionMailQuote(q) || EncodeCommissionMailQuote(q)!=text)return false;
        out=std::move(q);return true;
    } catch(const std::exception&) {return false;}
}
bool ExactCommissionMailConsumption(const Task& task,const CommissionMailQuote& q,const std::vector<ClaimConsumption>& uses) {
    CommissionJob job;ProfessionJob recipe;std::string why;
    if(!ValidCommissionMailQuote(q) || !ValidateCommissionTask(task,why) || !IsCommissionJob(task) ||
        !DecodeCommissionJob(task.checkpoint.data,job,why) || !job.craftFinishedRevision ||
        !DecodeProfessionIntent(job.craft,recipe,why) || task.actor!=q.sender || job.agreement.id!=q.commission ||
        job.agreement.recipient!=q.receiver || job.agreement.delivery!="mail" || job.agreement.feeCopper!=q.cod ||
        recipe.outputEntry!=q.entry || recipe.outputQuantity!=q.quantity || uses.size()!=2)return false;
    bool item=false,money=false;std::set<std::string> ids;
    for(const auto& use:uses) {
        const auto& c=use.before;
        if(!ValidResourceClaim(c) || c.task!=task.id || c.actor!=q.sender || c.state!="held" ||
            c.nativeReference || !ids.insert(c.id).second)return false;
        if(c.location=="bags" && c.itemGuid==q.item && c.itemEntry==q.entry && !c.copper &&
            c.quantity==q.quantity && use.used==q.quantity && !item)item=true;
        else if(c.location=="money" && !c.itemGuid && !c.itemEntry && !c.quantity &&
            c.copper==q.postage && use.used==q.postage && !money)money=true;
        else return false;
    }
    return item && money;
}
std::string CommissionMailSubject(const std::string& operation) {
    if(!IsUuid(operation))throw std::invalid_argument("commission_mail_operation_required");
    return "Commission delivery "+operation;
}
bool VerifyCommissionMailSent(const CommissionMailQuote& q,const AuctionMail& m,const std::string& operation) {
    return ValidCommissionMailQuote(q) && IsUuid(operation) && m.id && m.sender==q.sender && m.receiver==q.receiver &&
        !m.money && m.cod==q.cod && m.attachments==1 && m.itemGuid==q.item && m.itemEntry==q.entry &&
        m.quantity==q.quantity && m.deliveredAt && m.expiresAt>m.deliveredAt &&
        m.subject==CommissionMailSubject(operation);
}
std::string CommissionMailSentProof(const Task& task,const CommissionMailQuote& q,const AuctionMail& m,const std::string& operation) {
    if(task.actor!=q.sender || !VerifyCommissionMailSent(q,m,operation))return {};
    const auto n=[](uint64_t v){return std::to_string(v);};
    return "SELECT "+SqlValue(task.id)+','+n(task.revision)+" FROM mail m JOIN mail_items mi ON mi.mail_id=m.id"
        " JOIN item_instance i ON i.guid=mi.item_guid WHERE m.id="+n(m.id)+
        " AND m.messageType=0 AND m.sender="+n(q.sender)+" AND m.receiver="+n(q.receiver)+
        " AND m.money=0 AND m.cod="+n(q.cod)+" AND m.subject="+SqlValue(m.subject)+
        " AND m.deliver_time="+n(m.deliveredAt)+" AND m.expire_time="+n(m.expiresAt)+
        " AND mi.item_guid="+n(q.item)+" AND mi.item_template="+n(q.entry)+" AND mi.receiver="+n(q.receiver)+
        " AND i.owner_guid="+n(q.receiver)+" AND i.itemEntry="+n(q.entry)+" AND i.count="+n(q.quantity)+
        " AND (SELECT COUNT(*) FROM mail_items a WHERE a.mail_id=m.id)=1"
        " AND NOT EXISTS (SELECT 1 FROM character_inventory v WHERE v.item=i.guid)"
        " AND NOT EXISTS (SELECT 1 FROM guild_bank_item g WHERE g.item_guid=i.guid)"
        " AND NOT EXISTS (SELECT 1 FROM mail_items a WHERE a.item_guid=i.guid AND a.mail_id<>m.id)"
        " AND EXISTS (SELECT 1 FROM characters c WHERE c.guid="+n(q.sender)+" AND c.money="+n(q.moneyBefore-q.postage)+')';
}
}
