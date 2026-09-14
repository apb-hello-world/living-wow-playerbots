#include "LivingBotDiagnostics.h"
#include <cassert>

int main() {
    for(const char* query : {"state", "position", "tpos", "target", "hp", "combat", "strategy", "action", "travelpath"})
        assert(LivingActivity::IsReadOnlyBotDiagnostic(query));
    for(const char* mutation : {"", "values", "nc", "position teleport", "position teleport 0 1 2 3",
                               "combat reset", "strategy +dps", "action cast", "travelpath reset",
                               "setvalueuin32 health 999", "do spell", "state\nreset", " state", "state "})
        assert(!LivingActivity::IsReadOnlyBotDiagnostic(mutation));
}
