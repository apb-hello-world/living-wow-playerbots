#pragma once
#include "LivingCommissionJob.h"

namespace LivingActivity {
// One native metadata transaction accepts the exact contract AND its durable
// task. Neither admission nor a legacy expiry is proof of crafting/delivery.
inline WritePlan CommissionAdmissionWrite(const Task& task,const std::string& receipt) {
    CommissionJob job;ProfessionJob recipe;std::string why;
    if(!ValidateCommissionTask(task,why) || !DecodeCommissionJob(task.checkpoint.data,job,why) ||
        !DecodeProfessionJob(job.craft,recipe,why) || task.revision!=1 || task.phase!=Phase::Queued ||
        !task.accepted || task.mode!=Mode::Active || job.craftFinishedRevision || job.agreement.delivery!="mail")
        throw std::invalid_argument("new_mail_commission_required");
    const auto& c=job.agreement;
    const auto payload=EncodeCommissionContract(c),id=SqlValue(c.id);
    auto n=[](uint64_t value){return std::to_string(value);};
    const auto exact="commission_id="+id+" AND bot_guid="+n(c.actor)+" AND player_guid="+n(c.recipient)+
        " AND recipe_spell_id="+n(recipe.recipe)+" AND output_item_entry="+n(recipe.outputEntry)+
        " AND quantity="+n(recipe.outputQuantity)+" AND materials_source='bot' AND service_fee_copper="+n(c.feeCopper)+
        " AND state='crafting' AND authoritative_payload="+SqlValue(payload);
    auto plan=TaskWrite(task,0,receipt,"task_admitted");
    auto& insert=plan.statements.front();
    const auto values=insert.find(") VALUES ("),suffix=insert.rfind(") ON DUPLICATE KEY UPDATE task_id=task_id");
    if(values==std::string::npos || suffix==std::string::npos || suffix<=values)
        throw std::logic_error("commission_admission_insert_contract_changed");
    insert=insert.substr(0,values+1)+" SELECT "+insert.substr(values+10,suffix-(values+10))+
        " WHERE EXISTS(SELECT 1 FROM organic_economy_commission WHERE "+exact+") ON DUPLICATE KEY UPDATE task_id=task_id";
    // Compatibility schema requires a timestamp. This sentinel is deliberately
    // not an execution deadline: accepted managed work has no timer expiry.
    plan.statements.insert(plan.statements.begin(),
        "INSERT INTO organic_economy_commission(commission_id,bot_guid,player_guid,recipe_spell_id,output_item_entry,"
        "quantity,materials_source,service_fee_copper,state,authoritative_payload,expires_at) SELECT "+id+','+
        n(c.actor)+','+n(c.recipient)+','+n(recipe.recipe)+','+n(recipe.outputEntry)+','+n(recipe.outputQuantity)+
        ",'bot',"+n(c.feeCopper)+",'crafting',"+SqlValue(payload)+",'9999-12-31 23:59:59' WHERE NOT EXISTS"
        "(SELECT 1 FROM organic_economy_commission WHERE commission_id="+id+") AND NOT EXISTS"
        "(SELECT 1 FROM living_activity_task WHERE task_id="+SqlValue(task.id)+" OR (source='commission_job' AND source_key="+
        id+")) ON DUPLICATE KEY UPDATE commission_id=commission_id");
    plan.receiptQuery+=" AND EXISTS(SELECT 1 FROM organic_economy_commission WHERE "+exact+')';
    return plan;
}
}
