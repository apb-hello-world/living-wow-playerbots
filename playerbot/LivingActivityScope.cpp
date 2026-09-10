#include "LivingActivityScope.h"
#include <cassert>

namespace LivingActivity {
    thread_local EvaluationScope* EvaluationScope::head = nullptr;
    EvaluationScope::~EvaluationScope() { assert(head == this); head=previous; }
    bool EvaluationScope::RejectMutation(const Effects& effects) {
        if (!head || (effects.classified && !effects.mask)) return false;
        unsigned depth=0;
        for (auto* scope=head;scope && depth++ < 16;scope=scope->previous) scope->mutationAttempted=true;
        return true;
    }
    thread_local ExecutionScope* ExecutionScope::head = nullptr;
    ExecutionScope::ExecutionScope(Task supplied, ActionContext context)
        : previous(head), depth(previous ? previous->depth + 1 : 1), actor(supplied.actor),
          task(std::move(supplied)), action(std::move(context)) { head = this; }
    ExecutionScope::ExecutionScope(NativePermit supplied)
        : previous(head), depth(previous ? previous->depth + 1 : 1), actor(supplied.world.actor),
          permit(std::move(supplied)) { head = this; }
    ExecutionScope::~ExecutionScope() { assert(head == this); head = previous; }
    AuthorityCode ExecutionScope::Check(const PermissionReader& reader, const Effects& effects,
        const WorldContext& current, uint64_t now, uint32_t nativeSafety) {
        if (EvaluationScope::RejectMutation(effects)) return AuthorityCode::EffectsDenied;
        // A nested other-actor or over-depth call must not fall through to an
        // outer actor's authority. Absence of scope is never managed permission.
        const auto* scope = head && head->actor == current.actor && head->depth <= 16 ? head : nullptr;
        return reader.Check(effects, current, now,
            scope && scope->task ? &*scope->task : nullptr,
            scope && scope->action ? &*scope->action : nullptr,
            scope && scope->permit ? &*scope->permit : nullptr, nativeSafety);
    }
    std::string ExecutionScope::Origin(uint32_t actor) {
        if (EvaluationScope::Active()) return "eligibility_evaluation";
        if (!head || head->actor != actor || head->depth > 16) return "unscoped";
        if (head->permit) return "native_validated";
        return head->action && IsToken(head->action->origin) ? head->action->origin : "invalid_scope";
    }
}
