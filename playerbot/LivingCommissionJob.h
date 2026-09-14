#pragma once
#include "LivingCommissionContract.h"

namespace LivingActivity {
// The agreement is immutable; tool preparation and crafting progress belong to
// the existing profession workflow. A finished craft is NOT a paid commission.
struct CommissionJob {
    CommissionContract agreement;
    std::string craft;
    uint64_t craftFinishedRevision=0;
};
inline bool IsCommissionJob(const Task& task) {return task.source=="commission_job";}
inline bool DecodeCommissionJob(const std::string& data,CommissionJob& result,std::string& why) {
    result={};why="invalid_commission_job";
    if(data.empty() || data.size()>8192)return false;
    try {
        boost::property_tree::ptree p;std::istringstream input(data);boost::property_tree::read_json(input,p);
        const std::set<std::string> fields{"workflow","agreement","craft","craft_finished_revision"};
        std::set<std::string> seen;
        for(const auto& f:p)if(!fields.count(f.first) || !seen.insert(f.first).second)return false;
        if(seen!=fields || !p.get_child("workflow").empty() || p.get<std::string>("workflow")!="commission_job_v1")return false;
        auto json=[](const boost::property_tree::ptree& tree){std::ostringstream out;boost::property_tree::write_json(out,tree,false);return out.str();};
        CommissionJob job;
        if(!DecodeCommissionContract(json(p.get_child("agreement")),job.agreement,why))return false;
        // Disallow nested domain wrappers before recursively decoding the plain
        // profession workflow; malformed input cannot recurse indefinitely.
        const auto& craft=p.get_child("craft");
        if(craft.get<std::string>("workflow","")!="profession_job_v1")return false;
        job.craft=json(craft);ProfessionWorkflow flow,agreed;
        if(!DecodeProfessionWorkflow(job.craft,flow,why) || !DecodeProfessionWorkflow(job.agreement.recipe,agreed,why) ||
            EncodeProfessionJob(flow.intent)!=EncodeProfessionJob(agreed.intent))return false;
        const auto& revision=p.get_child("craft_finished_revision");const auto value=revision.data();
        if(!revision.empty() || value.empty() || value.size()>20 || value.find_first_not_of("0123456789")!=std::string::npos ||
            (value.size()>1 && value[0]=='0'))return false;
        job.craftFinishedRevision=std::stoull(value);
        result=std::move(job);why.clear();return true;
    } catch(const std::exception&) {return false;}
}
inline std::string EncodeCommissionJob(const CommissionJob& job) {
    boost::property_tree::ptree p,agreement,craft;
    std::istringstream a(EncodeCommissionContract(job.agreement)),c(job.craft);
    boost::property_tree::read_json(a,agreement);boost::property_tree::read_json(c,craft);
    p.put("workflow","commission_job_v1");p.add_child("agreement",agreement);p.add_child("craft",craft);
    p.put("craft_finished_revision",job.craftFinishedRevision);
    std::ostringstream out;boost::property_tree::write_json(out,p,false);
    CommissionJob checked;std::string why;
    if(!DecodeCommissionJob(out.str(),checked,why))throw std::invalid_argument(why);
    return out.str();
}
inline bool ValidateCommissionTask(const Task& task,std::string& why) {
    if(!IsCommissionJob(task)){why.clear();return true;}
    CommissionJob job;
    if(!DecodeCommissionJob(task.checkpoint.data,job,why))return false;
    if(task.kind!=Kind::Commission || task.actor!=job.agreement.actor || task.sourceKey!=job.agreement.id ||
        task.root!=task.id || !task.parent.empty() || job.craftFinishedRevision>task.revision ||
        task.phase==Phase::Completed) {why="commission_identity_or_settlement_invalid";return false;}
    why.clear();return true;
}
inline bool PreserveCommissionIntent(const Task& before,const Task& after,std::string& why) {
    if(!ValidateCommissionTask(after,why))return false;
    if(!before.accepted || !IsCommissionJob(before))return true;
    CommissionJob a,b;
    if(!IsCommissionJob(after) || !DecodeCommissionJob(before.checkpoint.data,a,why) ||
        !DecodeCommissionJob(after.checkpoint.data,b,why) ||
        EncodeCommissionContract(a.agreement)!=EncodeCommissionContract(b.agreement)) {
        why="accepted_commission_agreement_is_immutable";return false;
    }
    if(a.craftFinishedRevision!=b.craftFinishedRevision &&
        (a.craftFinishedRevision || b.craftFinishedRevision!=after.revision || after.revision!=before.revision+1 ||
         before.phase!=Phase::Verifying || after.phase!=Phase::Preparing || after.checkpoint.step!="commission_craft_ready")) {
        why="commission_craft_handoff_revision_invalid";return false;
    }
    why.clear();return true;
}
}
