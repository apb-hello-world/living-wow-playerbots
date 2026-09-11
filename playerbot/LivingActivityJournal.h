#ifndef LIVING_ACTIVITY_JOURNAL_H
#define LIVING_ACTIVITY_JOURNAL_H
#include "LivingActivity.h"

namespace LivingActivity { namespace Detail {
    // SQL construction only, for compiled domain journals. Callers MUST add
    // their native-proof/state predicates. No admission or execution authority
    // is granted here; ordinary producers continue to use guarded TaskWrite.
    WritePlan TaskTransitionWrite(const Task& task,uint64_t expectedRevision,
        const std::string& receipt,const std::string& code,const std::string& fingerprint);
}}
#endif
