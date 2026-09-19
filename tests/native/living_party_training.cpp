#include "LivingPartyTraining.h"
#include "LivingPartyService.h"
#include "LivingActivityRequests.h"
#include <cassert>
using namespace LivingActivity;
int main() {
    PartyTrainingJob job{123,0,{100,200}},decoded;
    const auto encoded=EncodePartyTrainingJob(job);
    assert(DecodePartyTrainingJob(encoded,decoded));
    for(const auto bad:{"{}","[]","", "null"})assert(!DecodePartyTrainingJob(bad,decoded));
    assert(!DecodePartyTrainingJob(encoded+"{}",decoded));
    auto changed=job;changed.lessons={100,100};assert(!ValidPartyTrainingJob(changed));
    changed=job;changed.next=3;assert(!ValidPartyTrainingJob(changed));
    changed=job;changed.lessons.clear();assert(!ValidPartyTrainingJob(changed));
    changed=job;changed.trainer=0;assert(!ValidPartyTrainingJob(changed));
    Task task;task.id=task.root="11111111-1111-4111-8111-111111111111";
    task.source="party_training";task.sourceKey="training:7:1";task.actor=7;task.kind=Kind::PartyErrand;
    task.mode=Mode::Active;task.accepted=true;task.phase=Phase::Preparing;task.revision=4;
    task.createdAtMs=task.updatedAtMs=1000;task.context.actor=7;
    task.context.actorGeneration=task.context.mapGeneration=task.context.policyRevision=1;
    task.context.boot="22222222-2222-4222-8222-222222222222";
    task.checkpoint.step="party_training_prepare";task.checkpoint.data=encoded;std::string why;
    assert(ValidatePartyTrainingTask(task,why));
    auto bad=task;bad.phase=Phase::Completed;assert(!ValidatePartyTrainingTask(bad,why));
    bad=task;bad.kind=Kind::Profession;assert(!ValidatePartyTrainingTask(bad,why));
    TaskRequest request;request.task=task;++request.task.revision;request.expectedRevision=task.revision;request.receipt=task.context.boot;
    assert(ValidateTaskRequest(request,&task,task.context,why)==AdmissionCode::Pending);
    changed=job;changed.next=1;request.task.checkpoint.data=EncodePartyTrainingJob(changed);
    assert(ValidateTaskRequest(request,&task,task.context,why)==AdmissionCode::InvalidRequest);
    TrainingLessonQuote q;q.actor=7;q.trainer=321;q.lesson=q.teachingSpell=100;q.playerSpells={100};q.money=50;
    TrainingLessonQuote copy;assert(DecodeDirectTrainingQuote(EncodeDirectTrainingQuote(q),copy));
    assert(SameTrainingLessonQuote(q,copy) && PartyTrainingQuoteMatches(task,q));
    auto invalid=q;invalid.cost=1;assert(!DirectFreeTrainingQuote(invalid));
    invalid=q;invalid.cast=true;assert(!DirectFreeTrainingQuote(invalid));
    invalid=q;invalid.playerSpells={100,200};assert(!DirectFreeTrainingQuote(invalid));
    invalid=q;invalid.actor=8;assert(!PartyTrainingQuoteMatches(task,invalid));
    OperationResult proof;proof.id=task.context.boot;proof.task=task.id;proof.taskRevision=4;
    proof.kind="party_training_learn";proof.state=OperationState::Verified;
    proof.nativeReference="trainer_lesson:100";proof.evidence="native_training_exact_spellbook_and_unchanged_money";
    auto after=task;++after.revision;after.phase=Phase::Verifying;after.checkpoint.step="party_training_learn";
    bad=after;auto failed=proof;failed.state=OperationState::Rejected;
    assert(!AcknowledgePartyTraining(bad,q,failed));
    assert(AcknowledgePartyTraining(after,q,proof));
    assert(after.phase==Phase::Verifying && DecodePartyTrainingJob(after.checkpoint.data,decoded) && decoded.next==1);
    assert(!AcknowledgePartyTraining(after,q,proof));
    auto journal=OperationOutcomeWrite(after,4,proof,task.id,"{}");assert(!journal.statements.empty());
    q.lesson=q.teachingSpell=200;q.playerSpells={200};proof.taskRevision=5;proof.nativeReference="trainer_lesson:200";
    ++after.revision;assert(AcknowledgePartyTraining(after,q,proof));
    assert(after.phase==Phase::Completed);
    journal=OperationOutcomeWrite(after,5,proof,task.id,"{}");assert(!journal.statements.empty());
    failed=proof;failed.nativeReference="trainer_lesson:100";bool threw=false;
    try{OperationOutcomeWrite(after,5,failed,task.id,"{}");}catch(...){threw=true;}assert(threw);
    PartyServiceBinding binding;binding.root=task.id;binding.actor=7;binding.human=8;
    binding.session="group:3:4";binding.sessionRevision=2;binding.acceptedRevision=4;
    binding.service=PartyServiceBinding::Service::Training;
    PartyServiceWindow window{7,8,"group:3:4",2,true};
    assert(PartyServiceMatches(binding,task,window));window.authorized=false;
    assert(!PartyServiceMatches(binding,task,window));
    assert(PartyServiceEffects(Mask(Effect::Spell)|Mask(Effect::Social),binding.service));
    assert(!PartyServiceEffects(Mask(Effect::Money),binding.service));
    assert(!PartyServiceEffects(Mask(Effect::Inventory),binding.service));
    assert(PartyServiceOperation(binding,"party_training_learn",{}));
    assert(!PartyServiceOperation(binding,"profession_craft",{}));
    assert(PartyServiceReceipt(binding,after,"party_training_learn",{},proof.id));
    assert(!PartyServiceReceipt(binding,after,"party_training_learn",{},proof.id));
}
