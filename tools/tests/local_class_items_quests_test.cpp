// Host runner for the local gameplay self-test (src/game/local_selftest.cpp).
// With a DBC directory it imports the client's spells exactly as the
// application does and runs the class ability checks as well.
#include "game/local_selftest.hpp"
#include "game/local_spell_import.hpp"
#include "pipeline/dbc_loader.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: local_class_items_quests_test <world.json> <catalog dir> [DBFilesClient dir]\n";
        return 2;
    }
    std::vector<wowee::game::LocalSpellDefinition> spells;
    const bool withDbc = argc == 4 && argv[3][0];
    if (withDbc) {
        std::map<std::string, std::vector<uint8_t>> bytes;
        std::map<std::string, wowee::pipeline::DBCFile> files;
        for (const auto* name : {"Spell", "SpellRange", "SpellCastTimes", "SpellDuration", "SpellIcon", "SpellRadius",
                                 "SpellRuneCost", "SkillLine", "SkillLineAbility", "Talent", "TalentTab"}) {
            std::ifstream f(std::string(argv[3]) + "/" + name + ".dbc", std::ios::binary);
            bytes[name] = {std::istreambuf_iterator<char>(f), {}};
            if (!files[name].load(bytes[name])) { std::cerr << "cannot load " << name << ".dbc\n"; return 2; }
        }
        const auto get = [&](const char* n) { return &files.at(n); };
        auto imported = wowee::game::importClientStarterSpells(get("Spell"), get("SpellRange"), get("SpellCastTimes"),
            get("SpellDuration"), get("SpellIcon"), get("SkillLineAbility"), get("SkillLine"), get("Talent"),
            get("SpellRuneCost"), get("SpellRadius"));
        wowee::game::detail::importClientTalents(imported, get("Talent"), get("TalentTab"), get("Spell"), get("SpellRange"),
            get("SpellCastTimes"), get("SpellDuration"), get("SpellIcon"), get("SpellRuneCost"), get("SpellRadius"));
        spells = std::move(imported.spells);
    }
    return wowee::game::runLocalGameplaySelfTest(argv[1], argv[2], std::cout, true, withDbc ? &spells : nullptr) ? 0 : 1;
}
