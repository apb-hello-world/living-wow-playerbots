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
    bool ExecutionScope::Matches(const Task& task, const ActionContext& action) {
        return head && head->depth<=16 && head->actor==task.actor && head->task && head->action && !head->permit &&
            head->task->id==task.id && head->task->root==task.root && head->task->revision==task.revision &&
            head->task->context==task.context && head->action->task==action.task &&
            head->action->rootTask==action.rootTask && head->action->revision==action.revision &&
            head->action->ownerGeneration==action.ownerGeneration && head->action->world==action.world &&
            head->action->permittedEffects==action.permittedEffects && head->action->operation==action.operation;
    }
    Effects ExecutionScope::MutationEffects(uint32_t actor,uint32_t mask) {
        Effects result{mask,Lane::Managed,true};
        if (head && head->actor == actor && head->depth <= 16 && head->permit &&
            head->permit->validated && !(mask & ~head->permit->effects)) result.lane=head->permit->lane;
        return result; // The normal boundary still checks context, safety and effects.
    }
    bool ExecutionScope::RequiresNativeSpellItems(uint32_t actor) {
        if (!head || head->actor!=actor || head->depth>16 || !head->task || !head->action || head->permit)
            return false;
        const auto& task=*head->task;const auto& action=*head->action;
        const auto required=Mask(Effect::Spell)|Mask(Effect::Inventory);
        return task.mode==Mode::Active && task.accepted && task.phase==Phase::Executing &&
            IsUuid(action.operation) && !(required&~action.permittedEffects) && Fresh(task,action,task.context);
    }
    bool ExecutionScope::OwnsNativeOperation(uint32_t actor) {
        if(EvaluationScope::Active() || !head || head->actor!=actor || head->depth>16 || !head->task || !head->action || head->permit)
            return false;
        const auto& task=*head->task;const auto& action=*head->action;
        return task.mode==Mode::Active && task.accepted && task.phase==Phase::Executing &&
            IsUuid(action.operation) && (action.permittedEffects&(Mask(Effect::Inventory)|Mask(Effect::Money))) &&
            Fresh(task,action,task.context);
    }
    std::string ExecutionScope::NativeOperationId(uint32_t actor) {
        return OwnsNativeOperation(actor) ? head->action->operation : "";
    }
}
