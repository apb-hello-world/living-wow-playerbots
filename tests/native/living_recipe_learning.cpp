#include "LivingRecipeLearningSettlement.h"
#include "LivingActivityRequests.h"
#include "LivingProfessionJob.h"
#include "LivingTaskItemRequirements.h"
#include <cassert>
using namespace LivingActivity;
int main() {
    RecipeLearningJob job{6326,7752,185,483},decoded;std::string blocker;
    const auto encoded=EncodeRecipeLearningJob(job);
    assert(DecodeRecipeLearningJob(encoded,decoded,blocker) && EncodeRecipeLearningJob(decoded)==encoded);
    for (const auto bad : {"{}","[]","{\"workflow\":\"recipe_learning_v2\"}",
        "{\"workflow\":\"recipe_learning_v1\",\"book\":6326,\"book\":6325,\"recipe\":7752,\"skill\":185,\"teacher\":483}",
        "{\"workflow\":\"recipe_learning_v1\",\"book\":-1,\"recipe\":7752,\"skill\":185,\"teacher\":483}",
        "{\"workflow\":\"recipe_learning_v1\",\"book\":4294967296,\"recipe\":7752,\"skill\":185,\"teacher\":483}"})
        assert(!DecodeRecipeLearningJob(bad,decoded,blocker));
    auto invalid=job;invalid.teacher=invalid.recipe;assert(!ValidRecipeLearningJob(invalid));
    invalid=job;invalid.skill=43;assert(!ValidRecipeLearningJob(invalid));
    RecipeLearningResult result;
    result.before={79,440051,6326,1,0,28,586,1,75,false};
    result.after=result.before;result.after.count=0;result.after.bag=0;result.after.slot=0;result.after.known=true;
    result.started=result.effect=result.finished=result.succeeded=true;
    assert(VerifyRecipeLearning(job,result,blocker)==OperationState::Verified);
    for (unsigned fault=0;fault<11;++fault) {
        auto bad=result;
        switch(fault) {
            case 0: bad.started=false;break;case 1: bad.effect=false;break;case 2: bad.finished=false;break;
            case 3: bad.succeeded=false;break;case 4: bad.uncertain=true;break;case 5: bad.after.known=false;break;
            case 6: bad.before.known=true;break;case 7: bad.after.count=1;break;case 8: ++bad.after.guid;break;
            case 9: ++bad.after.money;break;case 10: ++bad.after.maximum;break;
        }
        assert(VerifyRecipeLearning(job,bad,blocker)==OperationState::Reconciling);
    }
    auto cancelled=result;cancelled.effect=cancelled.succeeded=false;cancelled.after=cancelled.before;
    assert(VerifyRecipeLearning(job,cancelled,blocker)==OperationState::Rejected);
    ++cancelled.after.skill;assert(VerifyRecipeLearning(job,cancelled,blocker)==OperationState::Reconciling);
    auto stacked=result;stacked.before.count=2;stacked.after.count=1;stacked.after.slot=stacked.before.slot;
    assert(VerifyRecipeLearning(job,stacked,blocker)==OperationState::Verified);
    ++stacked.after.bag;assert(VerifyRecipeLearning(job,stacked,blocker)==OperationState::Reconciling);

    Task task;task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f92";task.actor=task.context.actor=79;
    task.source="recipe_learning";task.sourceKey="actor:79:recipe:7752";task.mode=Mode::Active;
    task.kind=Kind::Profession;task.phase=Phase::Verifying;task.revision=6;task.createdAtMs=task.updatedAtMs=1000;
    task.checkpoint.step="recipe_learning";task.checkpoint.data=encoded;
    task.context.boot="69e55001-c9b0-4d34-9e15-4351e3e4af2a";
    task.context.actorGeneration=task.context.mapGeneration=task.context.policyRevision=1;
    assert(ValidateProfessionTask(task,blocker));
    std::vector<ProfessionReagent> items;
    assert(ReadTaskItemRequirements(task,items,blocker) && items==std::vector<ProfessionReagent>({{6326,1}}));
    auto service=task;service.checkpoint.step="profession_service_mail";
    assert(ValidateProfessionTask(service,blocker) && !IsProfessionJob(service) && IsRecipeLearningTask(service));
    assert(ReadTaskItemRequirements(service,items,blocker) && items.front().entry==6326);
    auto changed=task;changed.checkpoint.data=EncodeRecipeLearningJob({6325,7751,185,483});
    assert(!PreserveProfessionIntent(task,changed,blocker));
    changed=task;changed.source="other";changed.checkpoint.step="other";changed.checkpoint.data="{}";
    assert(!PreserveProfessionIntent(task,changed,blocker));
    changed=task;changed.checkpoint.step="recipe_completed";assert(PreserveProfessionIntent(task,changed,blocker));
    UnsettledClaimBatch claims;claims.bookRevision=1;claims.complete=true;RecipeLearningSettlement settlement;
    const std::string receipt="312bd562-36d2-5b01-bc01-e2d831c49f92";
    assert(PrepareRecipeLearningSettlement(task,task.context,claims,2000,receipt,settlement,blocker));
    assert(settlement.task.phase==Phase::Completed && settlement.task.revision==7);
    std::string sql;for (const auto& line:settlement.plan.statements) sql+=line;
    assert(sql.find("native_recipe_book_consumed_and_spell_learned")!=std::string::npos);
    assert(sql.find("character_spell")!=std::string::npos && sql.find("living_activity_claim")!=std::string::npos);
    assert(sql.find("o.state IN ('intent','reconciling')")!=std::string::npos);
    auto restart=task;restart.phase=Phase::Reconciling;auto current=task.context;++current.actorGeneration;
    assert(PrepareRecipeLearningSettlement(restart,current,claims,2000,receipt,settlement,blocker));
    assert(settlement.task.context==current);
    claims.complete=false;assert(!PrepareRecipeLearningSettlement(task,current,claims,2000,receipt,settlement,blocker));
    claims.complete=true;claims.claims.push_back({});
    assert(!PrepareRecipeLearningSettlement(task,current,claims,2000,receipt,settlement,blocker));
    claims.claims.clear();task.phase=Phase::Preparing;
    assert(!PrepareRecipeLearningSettlement(task,current,claims,2000,receipt,settlement,blocker));
    assert(PrepareRecipeLearningResumption(task,current,claims,{},2000,receipt,settlement,blocker));
    ResourceClaim original;original.id="ff2efbdf-f0ec-4539-b840-299847979001";original.task=task.id;original.actor=79;
    original.itemGuid=440051;original.itemEntry=6326;original.quantity=1;original.location="bags";original.state="held";
    claims.claims={original};std::vector<NativeResourceBalance> stock{{79,440051,6326,1,0,"bags"}};
    assert(PrepareRecipeLearningResumption(task,current,claims,stock,2000,receipt,settlement,blocker));
    ++stock[0].itemGuid;assert(!PrepareRecipeLearningResumption(task,current,claims,stock,2000,receipt,settlement,blocker));
    --stock[0].itemGuid;stock[0].location="bank";claims.claims[0].location="bank";
    assert(PrepareRecipeLearningResumption(task,current,claims,stock,2000,receipt,settlement,blocker));
    stock[0].location="mail";stock[0].nativeReference=8190;
    claims.claims[0].location="mail";claims.claims[0].nativeReference=8190;
    assert(PrepareRecipeLearningResumption(task,current,claims,stock,2000,receipt,settlement,blocker));
    ++stock[0].nativeReference;assert(!PrepareRecipeLearningResumption(task,current,claims,stock,2000,receipt,settlement,blocker));
    --stock[0].nativeReference;
    auto funds=original;funds.id="ff2efbdf-f0ec-4539-b840-299847979002";funds.itemGuid=funds.itemEntry=funds.quantity=0;
    funds.copper=50;funds.location="money";claims.claims.push_back(funds);
    stock.insert(stock.begin(),NativeResourceBalance{79,0,0,0,100,"money"});
    assert(PrepareRecipeLearningResumption(task,current,claims,stock,2000,receipt,settlement,blocker));
    stock[0].copper=49;assert(!PrepareRecipeLearningResumption(task,current,claims,stock,2000,receipt,settlement,blocker));
    stock[0].copper=100;task.phase=Phase::Verifying;task.checkpoint.step="profession_mail_collect";task.retryAtMs=2500;
    assert(PrepareRecipeLearningResumption(task,task.context,claims,stock,2000,receipt,settlement,blocker));
    assert(settlement.task.retryAtMs==2500);
    sql.clear();for (const auto& line:settlement.plan.statements) sql+=line;
    assert(sql.find("o.kind NOT IN ('vendor_purchase','mail_collect'")!=std::string::npos);
    assert(sql.find("OR o.state NOT IN ('verified','rejected')")!=std::string::npos);
    assert(sql.find("mail_items")!=std::string::npos && sql.find("m.cod=0")!=std::string::npos);
}
