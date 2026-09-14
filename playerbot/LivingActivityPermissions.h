#ifndef LIVING_ACTIVITY_PERMISSIONS_H
#define LIVING_ACTIVITY_PERMISSIONS_H
#include "LivingActivityAuthority.h"
#include <memory>

namespace LivingActivity {
    class PermissionPublisher;
    // Worker capability: read/check only. Possessing an ownership snapshot never
    // implicitly attaches that task to an autonomous action. A domain executor
    // must explicitly supply its validated scoped ActionContext.
    class PermissionReader {
    public:
        PermissionReader() = default;
        AuthorityCode Check(const Effects& effects, const WorldContext& current, uint64_t now,
            const Task* task = nullptr, const ActionContext* action = nullptr,
            const NativePermit* permit = nullptr, uint32_t nativeSafety = 0,
            uint32_t nativeBlockedEffects = AllEffects) const;
        std::shared_ptr<const AuthoritySnapshot> Inspect() const;
    private:
        friend class PermissionPublisher;
        struct Cell { std::shared_ptr<const AuthoritySnapshot> value; };
        explicit PermissionReader(std::shared_ptr<Cell> value) : cell(std::move(value)) {}
        std::shared_ptr<Cell> cell;
    };
    // Retained only by the world coordinator; not handed to a native map worker.
    class PermissionPublisher {
    public:
        PermissionPublisher() : cell(std::make_shared<PermissionReader::Cell>()) {}
        PermissionReader Reader() const { return PermissionReader(cell); }
        void Publish(const AuthoritySnapshot& value);
        void Revoke();
    private:
        std::shared_ptr<PermissionReader::Cell> cell;
    };
}
#endif
