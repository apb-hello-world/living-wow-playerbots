#pragma once
#include "LivingTrainingLesson.h"
#include <string>
class PlayerbotAI;
class ObjectGuid;
namespace LivingActivity {
struct NativeTrainingLessonResult {
    TrainingLessonOutcome outcome=TrainingLessonOutcome::Rejected;
    std::string reason;
};
// Re-reads the interacting trainer's actual list and all prerequisites. Does
// not grant missing prerequisites, supply money, or report aggregate learning.
// Planning is read-only. A durable caller can save this value-only quote before
// dispatch; execution re-quotes and requires an exact match under fresh authority.
bool PlanNativeTrainingLesson(PlayerbotAI&,const ObjectGuid& trainer,uint32_t lesson,TrainingLessonQuote&,std::string&);
NativeTrainingLessonResult ExecuteNativeTrainingLesson(PlayerbotAI&,const TrainingLessonQuote&);
NativeTrainingLessonResult ExecuteNativeTrainingLesson(PlayerbotAI&,const ObjectGuid& trainer,uint32_t lesson);
}
