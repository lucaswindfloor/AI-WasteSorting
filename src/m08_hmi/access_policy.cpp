#include "access_policy.h"

#include <algorithm>

namespace waste::m08 {

bool allows(const Principal& p, const Scope& scope, Permission permission) {
    return !p.subject.empty() && !scope.station.empty() && !scope.run.empty() &&
           std::ranges::any_of(p.grants, [&](const Grant& grant) {
               return grant.scope == scope && grant.permission == permission;
           });
}

bool allows(const Principal& p, const Scope& scope, Action action) {
    switch (action) {
    case Action::Start:
    case Action::Pause:
    case Action::Drain:
    case Action::Acknowledge: return allows(p, scope, Permission::Operate);
    case Action::ChangeBin: return allows(p, scope, Permission::ChangeBin);
    case Action::Approve: return allows(p, scope, Permission::Approve);
    case Action::Publish:
    case Action::Rollback: return allows(p, scope, Permission::Publish);
    }
    return false;
}

bool can_read(const Principal& p, const Operation& operation) {
    return allows(p, operation.request.key.scope, Permission::Audit) ||
           (p.subject == operation.actor && allows(p, operation.request.key.scope, Permission::View));
}

}  // namespace waste::m08
