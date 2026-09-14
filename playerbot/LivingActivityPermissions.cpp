#include "LivingActivityPermissions.h"
#include <atomic>

namespace LivingActivity {
    std::shared_ptr<const AuthoritySnapshot> PermissionReader::Inspect() const {
        return cell ? std::atomic_load_explicit(&cell->value, std::memory_order_acquire) : nullptr;
    }
    AuthorityCode PermissionReader::Check(const Effects& effects, const WorldContext& current, uint64_t now,
        const Task* task, const ActionContext* action, const NativePermit* permit, uint32_t nativeSafety,
        uint32_t nativeBlockedEffects) const {
        const auto view = Inspect(); // Reload at every effect boundary, not once per journey.
        return view ? ExecutionAuthority::Check(*view, effects, current, now, task, action, permit, nativeSafety,
            nativeBlockedEffects) : AuthorityCode::NoOwner;
    }
    void PermissionPublisher::Publish(const AuthoritySnapshot& value) {
        std::shared_ptr<const AuthoritySnapshot> immutable = std::make_shared<const AuthoritySnapshot>(value);
        std::atomic_store_explicit(&cell->value, std::move(immutable), std::memory_order_release);
    }
    void PermissionPublisher::Revoke() {
        std::atomic_store_explicit(&cell->value, std::shared_ptr<const AuthoritySnapshot>{}, std::memory_order_release);
    }
}
