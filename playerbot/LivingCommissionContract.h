#pragma once
#include "LivingProfessionJob.h"
#include <boost/property_tree/json_parser.hpp>
#include <set>
#include <sstream>

namespace LivingActivity {
// Accepted native offer, not proof of crafting, delivery, payment or a durable
// executor. Keeping the exact recipe and recipient makes later reconciliation
// possible without guessing from an item entry or a legacy timer.
struct CommissionContract {
    std::string id,transaction,delivery,recipe;
    uint32_t actor=0,recipient=0,feeCopper=0;
    uint64_t acceptedAtMs=0;
};
inline bool ValidCommissionContract(const CommissionContract& c,std::string& why) {
    auto reject=[&](){why="invalid_native_commission_contract";return false;};
    if(c.id.size()<5 || c.id.size()>36 || c.id.compare(0,4,"lwc-") ||
        c.id.find_first_not_of("0123456789",4)!=std::string::npos ||
        !IsSourceKey(c.transaction) || !c.actor || !c.recipient || c.actor==c.recipient ||
        !c.acceptedAtMs || (c.delivery!="direct" && c.delivery!="meeting" && c.delivery!="mail") ||
        c.recipe.empty() || c.recipe.size()>4096)return reject();
    try {
        boost::property_tree::ptree root;std::istringstream in(c.recipe);boost::property_tree::read_json(in,root);
        if(root.get<std::string>("workflow")!="profession_job_v1")return reject();
    } catch(const std::exception&) {return reject();}
    ProfessionWorkflow flow;
    if(!DecodeProfessionWorkflow(c.recipe,flow,why) || !flow.tools.empty() ||
        flow.intent.operation!=ProfessionOperation::CreateItem ||
        flow.intent.purpose!=ProfessionPurpose::RequestedItem || !flow.intent.outputQuantity || flow.intent.outputQuantity>10000 ||
        flow.intent.reagents.empty())return reject();
    why.clear();return true;
}
inline std::string EncodeCommissionContract(const CommissionContract& c) {
    std::string why;if(!ValidCommissionContract(c,why))throw std::invalid_argument(why);
    boost::property_tree::ptree root,recipe;std::istringstream in(c.recipe);
    boost::property_tree::read_json(in,recipe);
    root.put("schema",1);root.put("kind","native_craft_commission");root.put("id",c.id);
    root.put("transaction",c.transaction);root.put("actor",c.actor);root.put("recipient",c.recipient);
    root.put("delivery",c.delivery);root.put("fee_copper",c.feeCopper);root.put("accepted_at_ms",c.acceptedAtMs);
    root.add_child("recipe",recipe);
    std::ostringstream out;boost::property_tree::write_json(out,root,false);
    if(out.str().size()>8192)throw std::invalid_argument("commission_contract_bound");
    return out.str();
}
inline bool DecodeCommissionContract(const std::string& data,CommissionContract& c,std::string& why) {
    c={};why="invalid_native_commission_contract";
    if(data.empty() || data.size()>8192)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(data);boost::property_tree::read_json(in,p);
        const std::set<std::string> fields{"schema","kind","id","transaction","actor","recipient",
            "delivery","fee_copper","accepted_at_ms","recipe"};std::set<std::string> seen;
        for(const auto& f:p)if(!fields.count(f.first) || !seen.insert(f.first).second ||
            (f.first!="recipe" && !f.second.empty()))return false;
        if(seen!=fields || p.get<std::string>("schema")!="1" ||
            p.get<std::string>("kind")!="native_craft_commission" || !p.get_child("recipe").data().empty())return false;
        auto number=[&](const char* key,uint64_t maximum) {
            const auto s=p.get<std::string>(key);
            if(s.empty() || s.size()>20 || s.find_first_not_of("0123456789")!=std::string::npos ||
                (s.size()>1 && s[0]=='0'))throw std::invalid_argument("commission_number_invalid");
            const auto n=std::stoull(s);if(n>maximum)throw std::invalid_argument("commission_number_overflow");return n;
        };
        CommissionContract parsed;
        parsed.id=p.get<std::string>("id");parsed.transaction=p.get<std::string>("transaction");
        parsed.delivery=p.get<std::string>("delivery");parsed.actor=number("actor",UINT32_MAX);
        parsed.recipient=number("recipient",UINT32_MAX);parsed.feeCopper=number("fee_copper",UINT32_MAX);
        parsed.acceptedAtMs=number("accepted_at_ms",UINT64_MAX);
        std::ostringstream recipe;boost::property_tree::write_json(recipe,p.get_child("recipe"),false);parsed.recipe=recipe.str();
        if(!ValidCommissionContract(parsed,why))return false;
        c=parsed;why.clear();return true;
    } catch(const std::exception&) {return false;}
}
// Import the exact accepted agreement, never infer the recipient/recipe/fee
// from inventory or an expired legacy timer. This validates intent only; the
// native operation journal must separately reconcile crafting and settlement.
inline bool DecodeImportedCommission(const std::string& data,uint32_t actor,const std::string& id,
    CommissionContract& result,std::string& why) {
    result={};why="commission_contract_missing_or_invalid";
    if(data.empty() || data.size()>8192)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(data);boost::property_tree::read_json(in,p);
        std::set<std::string> seen;
        for(const auto& field:p)if(!seen.insert(field.first).second || !field.second.empty())return false;
        const auto raw=p.get<std::string>("authoritative_payload","");
        CommissionContract c;
        if(!DecodeCommissionContract(raw,c,why))return false;
        auto number=[&](const char* key) {
            const auto s=p.get<std::string>(key);
            if(s.empty() || s.size()>10 || s.find_first_not_of("0123456789")!=std::string::npos ||
                (s.size()>1 && s[0]=='0'))throw std::invalid_argument("commission_import_number_invalid");
            const auto n=std::stoull(s);if(n>UINT32_MAX)throw std::invalid_argument("commission_import_number_overflow");
            return uint32_t(n);
        };
        ProfessionWorkflow recipe;
        if(!DecodeProfessionWorkflow(c.recipe,recipe,why))return false;
        if(c.actor!=actor || c.id!=id || p.get<std::string>("commission_id")!=id ||
            number("player_guid")!=c.recipient || number("service_fee_copper")!=c.feeCopper ||
            number("recipe_spell_id")!=recipe.intent.recipe || number("output_item_entry")!=recipe.intent.outputEntry ||
            number("quantity")!=recipe.intent.outputQuantity || p.get<std::string>("materials_source")!="bot") {
            why="commission_contract_native_record_mismatch";return false;
        }
        result=std::move(c);why.clear();return true;
    } catch(const std::exception&) {why="commission_contract_missing_or_invalid";return false;}
}
}
