#include "LivingTrainingLesson.h"
#include <cassert>
using namespace LivingActivity;
int main() {
    TrainingLessonQuote quote;quote.actor=484;quote.trainer=123;quote.lesson=456;
    quote.money=100;quote.cost=10;quote.playerSpells={456};
    TrainingLessonState before;before.actor=484;before.money=100;before.playerSpells={111};
    assert(TrainingLessonReady(quote,before));
    auto after=before;after.money=90;after.playerSpells.insert(456);
    assert(ObserveTrainingLesson(quote,before,after)==TrainingLessonOutcome::Verified);
    assert(ObserveTrainingLesson(quote,before,before)==TrainingLessonOutcome::Rejected);
    after=before;after.playerSpells.insert(999);
    assert(ObserveTrainingLesson(quote,before,after)==TrainingLessonOutcome::Uncertain);
    after=before;after.money=90;
    assert(ObserveTrainingLesson(quote,before,after)==TrainingLessonOutcome::Uncertain);
    after=before;after.playerSpells.insert(456);
    assert(ObserveTrainingLesson(quote,before,after)==TrainingLessonOutcome::Uncertain);
    quote.cost=0;
    assert(ObserveTrainingLesson(quote,before,after)==TrainingLessonOutcome::Verified);
    assert(!TrainingLessonReady(quote,after)); // Already-known lessons cannot be replayed.
    auto wrong=before;wrong.actor=485;
    assert(!TrainingLessonReady(quote,wrong));
    assert(ObserveTrainingLesson(quote,before,wrong)==TrainingLessonOutcome::Uncertain);
    wrong=before;wrong.money=99;assert(!TrainingLessonReady(quote,wrong));
    quote.playerSpells={456,789};after=before;after.playerSpells.insert(456);
    assert(ObserveTrainingLesson(quote,before,after)==TrainingLessonOutcome::Uncertain);
    after.playerSpells.insert(789);
    assert(ObserveTrainingLesson(quote,before,after)==TrainingLessonOutcome::Verified);
    // A native lesson may teach a mix of already-known and missing targets.
    before.playerSpells.insert(456);
    assert(TrainingLessonReady(quote,before));
    assert(ObserveTrainingLesson(quote,before,after)==TrainingLessonOutcome::Verified);
    quote.playerSpells.clear();quote.pet=55;quote.petSpells={789};before.pet=55;
    after=before;after.playerSpells.insert(789);
    assert(ObserveTrainingLesson(quote,before,after)==TrainingLessonOutcome::Uncertain);
    after=before;after.petSpells.insert(789);
    assert(ObserveTrainingLesson(quote,before,after)==TrainingLessonOutcome::Verified);
    after.pet=56;
    assert(ObserveTrainingLesson(quote,before,after)==TrainingLessonOutcome::Uncertain);
    quote.pet=0;assert(!ValidTrainingLesson(quote));
    quote.pet=55;quote.petSpells={789,789};assert(!ValidTrainingLesson(quote));
    quote.petSpells={790,789};assert(!ValidTrainingLesson(quote));
    quote.petSpells={0};assert(!ValidTrainingLesson(quote));
    quote.petSpells={1,2,3,4};assert(!ValidTrainingLesson(quote));
    quote.petSpells.clear();assert(!ValidTrainingLesson(quote));
    quote.playerSpells={456};quote.cost=101;assert(!ValidTrainingLesson(quote));
    quote.cost=0;quote.trainer=0;assert(!ValidTrainingLesson(quote));
    quote.trainer=123;quote.lesson=0;assert(!ValidTrainingLesson(quote));
}
