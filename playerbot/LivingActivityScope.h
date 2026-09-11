#ifndef LIVING_ACTIVITY_SCOPE_H
#define LIVING_ACTIVITY_SCOPE_H
#include "LivingActivityPermissions.h"
#include <optional>

namespace LivingActivity {
    // Eligibility may inspect/cache values, but it is not an execution step.
    // This remains in force beneath nested native permits or managed scopes.
    // Observe mode reports attempted effects without changing legacy behavior.
    class EvaluationScope {
    public:
        explicit EvaluationScope(bool enforce) : previous(head), enforce(enforce) { head=this; }
        ~EvaluationScope();
        EvaluationScope(const EvaluationScope&) = delete;
        EvaluationScope& operator=(const EvaluationScope&) = delete;
        bool Rejected() const { return enforce && mutationAttempted; }
        static bool RejectMutation(const Effects& effects);
        static bool Active() { return head != nullptr; }
    private:
        static thread_local EvaluationScope* head;
        EvaluationScope* previous;
        bool enforce, mutationAttempted=false;
    };
    // Synchronous, explicitly supplied executor attribution. This is NOT a
    // grant: every boundary checks the world's latest immutable permission view.
    // Never infer this scope from the current owner or from an action name.
    // Value-only contents; no native pointers or references to the task cache.
    class ExecutionScope {
    public:
        ExecutionScope(Task task, ActionContext action);
        explicit ExecutionScope(NativePermit permit);
        ~ExecutionScope();
        ExecutionScope(const ExecutionScope&) = delete;
        ExecutionScope& operator=(const ExecutionScope&) = delete;
        static AuthorityCode Check(const PermissionReader& reader, const Effects& effects,
            const WorldContext& current, uint64_t now, uint32_t nativeSafety = 0);
        static std::string Origin(uint32_t actor);
        // Attribute a direct native helper to the explicitly supplied top
        // scope only. Never infer permission from the actor's current owner.
        static Effects MutationEffects(uint32_t actor,uint32_t mask);
        // Restriction only: explicit journaled spell operations may not use
        // Playerbots' virtual-item shortcut. This never grants native execution.
        static bool RequiresNativeSpellItems(uint32_t actor);
    private:
        static thread_local ExecutionScope* head;
        ExecutionScope* previous;
        unsigned depth;
        uint32_t actor;
        std::optional<Task> task;
        std::optional<ActionContext> action;
        std::optional<NativePermit> permit;
    };
}
#endif
