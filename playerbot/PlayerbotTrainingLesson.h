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
NativeTrainingLessonResult ExecuteNativeTrainingLesson(PlayerbotAI&,const ObjectGuid& trainer,uint32_t lesson);
}
