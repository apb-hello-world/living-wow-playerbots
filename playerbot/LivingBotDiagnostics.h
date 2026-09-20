#pragma once
#include <string>

namespace LivingActivity {
    // These exact console queries only inspect current native state. Debug
    // commands with arguments or mutations still go through normal action
    // authorization; a prefix match would accidentally admit teleport/reset.
    inline bool IsReadOnlyBotDiagnostic(const std::string& command) {
        return command=="state" || command=="position" || command=="tpos" ||
            command=="target" || command=="hp" || command=="combat" ||
            command=="strategy" || command=="action" || command=="travelpath" || command=="questtravel";
    }
}
