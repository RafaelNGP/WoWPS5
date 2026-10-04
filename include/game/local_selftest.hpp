#pragma once
#include <ostream>
#include <string>
#include <vector>

namespace wowee::game {
struct LocalSpellDefinition;
/// Checks the installed local realm content end to end through the real
/// gameplay commands: class equipment proficiencies, every race/class start's
/// gear, on-use consumables and (when `quests`) the start-zone quest chains.
/// Writes PASS/FAIL lines to `out`; returns false on the first broken rule.
/// `clientSpells` (the session's import of the player's Spell.dbc) enables the
/// class ability checks; null skips them.
bool runLocalGameplaySelfTest(const std::string& worldPath, const std::string& catalogDir, std::ostream& out, bool quests,
                              const std::vector<LocalSpellDefinition>* clientSpells = nullptr);
}
