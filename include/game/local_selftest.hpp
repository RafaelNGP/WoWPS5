#pragma once
#include <ostream>
#include <string>

namespace wowee::game {
/// Checks the installed local realm content end to end through the real
/// gameplay commands: class equipment proficiencies, every race/class start's
/// gear, on-use consumables and (when `quests`) the start-zone quest chains.
/// Writes PASS/FAIL lines to `out`; returns false on the first broken rule.
bool runLocalGameplaySelfTest(const std::string& worldPath, const std::string& catalogDir, std::ostream& out, bool quests);
}
