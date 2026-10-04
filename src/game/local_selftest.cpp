// Runtime self-test (host suite and WOWEE_DEV_SELFTEST on the console).
// Production-content checks for the three player-facing rules most easily
// broken by catalog or rule changes: class equipment proficiencies, on-use
// consumables and the start-zone quest chains of every race/class start.
#include "game/local_selftest.hpp"
#include "game/local_gameplay.hpp"
#include "game/local_auction_catalog.hpp"
#include "game/local_equipment.hpp"
#include "game/local_melee.hpp"
#include "game/local_pet.hpp"
#include "game/local_world_catalog.hpp"
#include "game/local_inventory_layout.hpp"
#include "game/local_quest_marker.hpp"
#include "game/local_quest_eligibility.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <vector>
namespace wowee::game {
#define SELFTEST_CHECK(x) do { if (!(x)) { out << "FAIL line " << __LINE__ << ": " #x "\n"; return false; } } while (0)

namespace {
struct Spawn { uint32_t entry, map; float x, y, z; };
std::multimap<uint32_t, Spawn> readSpawns(const std::string& path) {
    std::ifstream in(path, std::ios::binary); std::multimap<uint32_t, Spawn> out; unsigned char row[28];
    while (in.read(reinterpret_cast<char*>(row), 28)) {
        Spawn s{}; std::memcpy(&s.entry, row + 4, 4); std::memcpy(&s.map, row + 8, 4);
        std::memcpy(&s.x, row + 12, 4); std::memcpy(&s.y, row + 16, 4); std::memcpy(&s.z, row + 20, 4);
        out.emplace(s.entry, s);
    }
    return out;
}
// First catalog item of an item class/subclass usable at level 1 by everyone.
uint32_t findItem(uint8_t itemClass, uint8_t subClass, uint8_t inventoryType, const LocalWorldContent& c) {
    for (const auto& m : kLocalAuctionItems)
        if (m.itemClass == itemClass && m.subClass == subClass && m.requiredLevel <= 1 &&
            (!m.allowableClasses || m.allowableClasses == UINT32_MAX || (m.allowableClasses & 0x5ff) == 0x5ff))
            if (const auto* d = c.item(m.id); d && (!inventoryType || d->inventoryType == inventoryType)) return m.id;
    return 0;
}
const char* kClassNames[] = {"", "Warrior", "Paladin", "Hunter", "Rogue", "Priest", "DeathKnight", "Shaman", "Mage", "Warlock", "", "Druid"};
}

bool runLocalGameplaySelfTest(const std::string& worldPath, const std::string& catalogDir, std::ostream& out, bool quests,
                              const std::vector<LocalSpellDefinition>* clientSpells) {
    LocalGameplay game; std::string error, result;
    if (!game.loadContent(worldPath, error)) { out << error << "\n"; return false; }
    const auto& c = game.content(); SELFTEST_CHECK(c.catalog);
    const auto spawns = readSpawns(catalogDir + "/spawns.pack");

    // ---- 1. Class proficiencies through the real EquipItem command.
    const uint32_t plate = findItem(4, 4, 5, c), mail = findItem(4, 3, 5, c), leather = findItem(4, 2, 5, c), cloth = findItem(4, 1, 5, c);
    const uint32_t shield = findItem(4, 6, 14, c), sword2h = findItem(2, 8, 17, c), wand = findItem(2, 19, 26, c), dagger = findItem(2, 15, 13, c), sword = findItem(2, 7, 13, c);
    SELFTEST_CHECK(plate && mail && leather && cloth && shield && sword2h && wand && dagger && sword);
    struct Case { uint8_t cls, level; uint32_t item; bool ok; uint8_t slot; };
    const Case cases[] = {
        {8, 80, plate, false, 255}, {8, 80, mail, false, 255}, {8, 80, leather, false, 255}, {8, 1, cloth, true, 255},
        {1, 39, plate, false, 255}, {1, 40, plate, true, 255}, {2, 40, plate, true, 255}, {6, 55, plate, true, 255},
        {3, 39, mail, false, 255}, {3, 40, mail, true, 255}, {7, 40, mail, true, 255}, {4, 80, mail, false, 255},
        {11, 80, leather, true, 255}, {11, 80, mail, false, 255}, {5, 80, leather, false, 255},
        {1, 1, shield, true, 255}, {7, 1, shield, true, 255}, {8, 80, shield, false, 255}, {4, 80, shield, false, 255},
        {1, 1, sword2h, true, 255}, {8, 80, sword2h, false, 255}, {5, 80, sword2h, false, 255},
        {5, 1, wand, true, 255}, {8, 1, wand, true, 255}, {1, 80, wand, false, 255},
        // Off hand weapons need Dual Wield (slot index 16).
        {8, 80, dagger, false, 16}, {4, 19, dagger, false, 16}, {4, 20, dagger, true, 16}, {6, 55, dagger, false, 255}, {6, 55, sword, true, 16}, {5, 80, sword, false, 255}, {8, 1, sword, true, 255}, {1, 20, dagger, true, 16},
    };
    size_t checked = 0;
    for (const auto& k : cases) {
        LocalRealmPlayer p; p.guid = 1; p.race = k.cls == 6 || k.cls == 1 || k.cls == 4 || k.cls == 8 || k.cls == 2 || k.cls == 5 ? 1 : k.cls == 7 ? 11 : k.cls == 11 ? 4 : 1;
        p.classId = k.cls; p.level = k.level; p.gameplayInitialized = true; p.health = p.maxHealth = 100;
        p.inventory = {{k.item, 1, 0}};
        std::vector<LocalRealmPlayer*> players{&p};
        const bool ok = game.execute(p, {LocalAction::EquipItem, k.slot == 255 ? 0ULL : uint64_t(k.slot) + 1, k.item}, players, result);
        const bool worn = std::count(p.equipment.begin(), p.equipment.end(), k.item) > 0;
        if (ok != k.ok || worn != k.ok) {
            out << "FAIL equip " << kClassNames[k.cls] << " L" << unsigned(k.level) << " item " << k.item << " expected " << k.ok << " got " << ok << " (" << result << ")\n";
            return false;
        }
        ++checked;
    }
    // The refusal says why, in the original client's words.
    {
        const auto reason = [&](uint8_t cls, uint8_t level, uint32_t item, uint64_t target) {
            LocalRealmPlayer p; p.guid = 1; p.race = 1; p.classId = cls; p.level = level; p.gameplayInitialized = true;
            p.inventory = {{item, 1, 0}}; std::vector<LocalRealmPlayer*> players{&p};
            game.execute(p, {LocalAction::EquipItem, target, item}, players, result); return result;
        };
        SELFTEST_CHECK(reason(8, 80, plate, 0).find("proficiency") != std::string::npos);
        SELFTEST_CHECK(reason(1, 39, plate, 0).find("proficiency") != std::string::npos);
        SELFTEST_CHECK(reason(8, 80, dagger, 17).find("dual wield") != std::string::npos);
    }
    // A one-hand weapon offered to a class without Dual Wield lands in the main hand.
    {
        LocalRealmPlayer p; p.guid = 1; p.race = 1; p.classId = 8; p.level = 80; p.gameplayInitialized = true;
        p.inventory = {{dagger, 1, 0}}; std::vector<LocalRealmPlayer*> players{&p};
        SELFTEST_CHECK(game.execute(p, {LocalAction::EquipItem, 0, dagger}, players, result));
        SELFTEST_CHECK(p.equipment[localEquipmentIndex(LocalEquipmentSlot::MainHand)] == dagger && !p.equipment[localEquipmentIndex(LocalEquipmentSlot::OffHand)]);
    }
    out << "PASS class proficiencies: " << checked << " armor/weapon/shield/wand/dual-wield cases through EquipItem\n";

    // ---- 2. Every catalog start: legal starting weapon, consumables.
    size_t starts = 0;
    for (const auto& start : c.catalog->starts()) {
        LocalRealmPlayer p; p.guid = 100 + starts; p.race = start.race; p.classId = start.classId; p.name = "T";
        game.initializePlayer(p, true, 0);
        for (size_t slot = 0; slot < p.equipment.size(); ++slot) if (p.equipment[slot])
            if (const auto* m = localAuctionMetadata(p.equipment[slot]); m && !localClassCanUseItem(p.classId, p.level, m->itemClass, m->subClass)) {
                out << "FAIL start " << kClassNames[p.classId] << " wears unusable item " << p.equipment[slot] << "\n"; return false;
            }
        const bool hasWeapon = p.equipment[localEquipmentIndex(LocalEquipmentSlot::MainHand)] != 0;
        if (!hasWeapon) { out << "FAIL start race " << unsigned(p.race) << " " << kClassNames[p.classId] << " has no main-hand weapon\n"; return false; }
        ++starts;
    }
    out << "PASS starts: " << starts << " race/class starts wear only usable gear and hold a weapon\n";

    {
        SELFTEST_CHECK(c.consumables.size() > 500);
        const auto* potion = c.consumable(118); const auto* jerky = c.consumable(117); const auto* water = c.consumable(159);
        const auto* apple = c.consumable(4536); const auto* bandage = c.consumable(1251); const auto* runic = c.consumable(33447);
        SELFTEST_CHECK(potion && potion->instantHealth == 80 && potion->category == 4 && potion->categoryCooldownMs == 60000);
        SELFTEST_CHECK(jerky && jerky->regenHealth == 61 && jerky->durationMs == 18000 && jerky->noCombat && jerky->cancelOnMove);
        SELFTEST_CHECK(water && water->regenMana == 151 && apple && apple->regenHealth == 61);
        SELFTEST_CHECK(bandage && bandage->regenHealth == 66 && bandage->cancelOnDamage && bandage->categoryCooldownMs == 60000);
        SELFTEST_CHECK(runic && runic->requiredLevel == 70);
        LocalRealmPlayer p; p.guid = 7; p.race = 1; p.classId = 8; p.name = "Mage"; game.initializePlayer(p, true, 10);
        std::vector<LocalRealmPlayer*> players{&p};
        std::erase_if(p.inventory, [](const auto& s) { return s.itemId == 118; });
        p.inventory.push_back({118, 3, 20}); p.inventory.push_back({4536, 2, 21}); p.inventory.push_back({33447, 1, 22}); p.inventory.push_back({1251, 2, 19});
        normalizeLocalInventory(p);
        const auto count = [&](uint32_t id) { uint32_t n = 0; for (const auto& s : p.inventory) if (s.itemId == id) n += s.count; return n; };
        // Full health: refused, nothing consumed.
        SELFTEST_CHECK(!game.execute(p, {LocalAction::UseItem, 0, 118}, players, result) && count(118) == 3);
        p.health = 10; SELFTEST_CHECK(game.execute(p, {LocalAction::UseItem, 0, 118}, players, result));
        SELFTEST_CHECK(p.health == std::min(p.maxHealth, 90u) && count(118) == 2);
        // Shared potion cooldown.
        p.health = 10; SELFTEST_CHECK(!game.execute(p, {LocalAction::UseItem, 0, 118}, players, result) && count(118) == 2);
        // Level requirement.
        SELFTEST_CHECK(!game.execute(p, {LocalAction::UseItem, 0, 33447}, players, result) && count(33447) == 1);
        // Food: spread over 18 s, interrupted by moving.
        p.health = 10; SELFTEST_CHECK(game.execute(p, {LocalAction::UseItem, 0, 4536}, players, result) && p.health == 10 && count(4536) == 1);
        game.tick(0.1f, players);
        // The meal shows as the player's Food buff, named and iconed for the interface.
        SELFTEST_CHECK(std::any_of(p.healingAuras.begin(), p.healingAuras.end(), [](const auto& a) { return a.spellId == 433 && a.durationMs == 18000; }));
        SELFTEST_CHECK(c.consumableSpell(433) && c.consumableSpell(433)->name == "Food" && !c.consumableSpell(433)->iconPath.empty());
        for (int i = 0; i < 89; ++i) game.tick(0.1f, players);
        SELFTEST_CHECK(p.consumableRegens.size() == 1 && p.consumableRegens[0].givenHealth >= 29 && p.consumableRegens[0].givenHealth <= 32);
        p.x += 3; game.tick(0.1f, players); p.x -= 3;
        SELFTEST_CHECK(p.consumableRegens.empty()); // Standing up ends the meal.
        // Out-of-combat regeneration also runs, so compare against a fast without food.
        p.health = 10; for (int i = 0; i < 200; ++i) game.tick(0.1f, players); const uint32_t fasting = p.health;
        p.health = 10; SELFTEST_CHECK(game.execute(p, {LocalAction::UseItem, 0, 4536}, players, result));
        for (int i = 0; i < 200; ++i) game.tick(0.1f, players);
        SELFTEST_CHECK(p.consumableRegens.empty() && p.health >= std::min(p.maxHealth, fasting + 61));
        // Bandage: heal over time, Recently Bandaged blocks a second one.
        p.health = 10; SELFTEST_CHECK(game.execute(p, {LocalAction::UseItem, 0, 1251}, players, result));
        SELFTEST_CHECK(!game.execute(p, {LocalAction::UseItem, 0, 1251}, players, result) && count(1251) == 1);
        // Stat buffs: elixir, scroll, flask, Well Fed; one per kind.
        {
            LocalRealmPlayer b; b.guid = 8; b.race = 1; b.classId = 1; b.name = "Buffed"; game.initializePlayer(b, true, 60);
            std::vector<LocalRealmPlayer*> bp{&b};
            b.inventory = {{2454, 2, 0}, {3013, 1, 1}, {13510, 1, 2}, {2680, 2, 3}, {955, 1, 4}};
            normalizeLocalInventory(b);
            const auto melee0 = localMeleeStats(b, c); const auto armor0 = localMeleeArmor(b, c); const auto health0 = b.maxHealth;
            SELFTEST_CHECK(game.execute(b, {LocalAction::UseItem, 0, 2454}, bp, result));
            SELFTEST_CHECK(localMeleeStats(b, c).attributes[0] == melee0.attributes[0] + 4);
            SELFTEST_CHECK(localMeleeStats(b, c).attackPower > melee0.attackPower);
            SELFTEST_CHECK(game.execute(b, {LocalAction::UseItem, 0, 3013}, bp, result) && localMeleeArmor(b, c) > armor0);
            // Elixirs and flasks share a three second category cooldown.
            SELFTEST_CHECK(!game.execute(b, {LocalAction::UseItem, 0, 13510}, bp, result));
            for (int i = 0; i < 31; ++i) game.tick(0.1f, bp);
            SELFTEST_CHECK(game.execute(b, {LocalAction::UseItem, 0, 13510}, bp, result) && b.maxHealth == health0 + 400);
            // A second scroll replaces the first (one scroll at a time).
            SELFTEST_CHECK(game.execute(b, {LocalAction::UseItem, 0, 955}, bp, result) && localMeleeArmor(b, c) == armor0);
            SELFTEST_CHECK(b.consumableBuffs.size() == 3);
            // Well Fed after ten seconds of eating.
            b.health = b.maxHealth / 2;
            SELFTEST_CHECK(game.execute(b, {LocalAction::UseItem, 0, 2680}, bp, result));
            for (int i = 0; i < 95; ++i) game.tick(0.1f, bp);
            SELFTEST_CHECK(b.consumableBuffs.size() == 3);
            for (int i = 0; i < 15; ++i) game.tick(0.1f, bp);
            SELFTEST_CHECK(b.consumableBuffs.size() == 4 && std::any_of(b.consumableBuffs.begin(), b.consumableBuffs.end(), [](const auto& x) { return x.spellId == 19705; }));
            SELFTEST_CHECK(std::any_of(b.healingAuras.begin(), b.healingAuras.end(), [](const auto& a) { return a.spellId == 13510 || a.spellId == 17626; }));
            SELFTEST_CHECK(c.consumableSpell(19705) && c.consumableSpell(19705)->name == "Well Fed");
        }
        out << "PASS consumables: " << c.consumables.size() << " items; instant potion, shared cooldown, level gate, food over time, move interrupt, bandage debuff, elixir/scroll/flask/Well Fed buffs\n";
    }

    // ---- 2b. Hunter pets: tame a real Shadowglen beast, dismiss, call, refusals.
    {
        SELFTEST_CHECK(c.tameableFamily(2031) == 2 && !c.tameableFamily(2079) && c.spell(kLocalTameBeast) && c.spell(kLocalCallPet));
        LocalGameplay world; SELFTEST_CHECK(world.loadContent(worldPath, error));
        LocalRealmPlayer p; p.guid = 77; p.race = 4; p.classId = 3; p.name = "Huntress";
        world.initializePlayer(p, true, 10);
        for (const uint32_t id : {kLocalTameBeast, kLocalCallPet, kLocalDismissPet, kLocalRevivePet})
            if (std::find(p.knownSpells.begin(), p.knownSpells.end(), id) == p.knownSpells.end()) p.knownSpells.push_back(id);
        std::vector<LocalRealmPlayer*> players{&p};
        const auto livePet = [&]() -> const LocalRealmPet* {
            for (const auto& v : world.pets()) if (v.ownerGuid == p.guid) return &v; return nullptr; };
        // Nearest Young Nightsaber spawn to the start, streamed in.
        const Spawn* spot = nullptr; float best = 1e30f;
        for (auto [it, end] = spawns.equal_range(2031); it != end; ++it)
            if (it->second.map == p.mapId) { const float d = std::hypot(it->second.x - p.x, it->second.y - p.y); if (d < best) { best = d; spot = &it->second; } }
        SELFTEST_CHECK(spot);
        p.x = spot->x; p.y = spot->y; p.z = spot->z; ++p.positionRevision;
        const LocalRealmNpc* beast = nullptr;
        for (int i = 0; i < 200 && !beast; ++i) { world.tick(0.05f, players);
            for (const auto& n : world.npcs()) if (n.entry == 2031 && !n.dead && std::hypot(n.x - p.x, n.y - p.y) < 25) { beast = &n; break; } }
        SELFTEST_CHECK(beast);
        const uint64_t beastGuid = beast->guid;
        // A warrior cannot tame; a hunter can.
        { auto warrior = p; warrior.classId = 1; std::vector<LocalRealmPlayer*> w{&warrior};
          SELFTEST_CHECK(!world.execute(warrior, {LocalAction::CastSpell, beastGuid, kLocalTameBeast}, w, result)); }
        const auto finishCast = [&] { for (int i = 0; i < 500 && p.castingSpellId; ++i) { p.health = p.maxHealth; world.tick(0.05f, players); } };
        // Tame Beast is a 20 second channel; moving breaks it.
        SELFTEST_CHECK(world.execute(p, {LocalAction::CastSpell, beastGuid, kLocalTameBeast}, players, result) && p.castingSpellId == kLocalTameBeast);
        SELFTEST_CHECK(p.castTotalMs >= 15000);
        world.tick(0.5f, players); p.x += 2; world.tick(0.05f, players); p.x -= 2;
        SELFTEST_CHECK(!p.castingSpellId && !p.hunterPet.entry);
        p.globalCooldownMs = 0;
        SELFTEST_CHECK(world.execute(p, {LocalAction::CastSpell, beastGuid, kLocalTameBeast}, players, result));
        finishCast();
        world.tick(0.05f, players);
        SELFTEST_CHECK(p.hunterPet.entry == 2031 && p.hunterPet.family == 2 && p.hunterPet.active && !p.hunterPet.dead);
        const auto* pet = livePet();
        SELFTEST_CHECK(pet && pet->entry == 2031 && pet->summonSpellId == kLocalCallPet && pet->level == p.level && pet->resourceType == 2 && pet->maxHealth > 50);
        SELFTEST_CHECK(pet->name == "Young Nightsaber");
        // The tamed beast left the world without loot.
        for (const auto& n : world.npcs()) if (n.guid == beastGuid) SELFTEST_CHECK(n.dead && !n.lootable);
        // One pet: a second tame is refused, Revive needs a dead pet.
        SELFTEST_CHECK(!world.execute(p, {LocalAction::CastSpell, beastGuid, kLocalTameBeast}, players, result));
        SELFTEST_CHECK(!world.execute(p, {LocalAction::CastSpell, 0, kLocalRevivePet}, players, result));
        p.globalCooldownMs = 0;
        SELFTEST_CHECK(world.execute(p, {LocalAction::CastSpell, 0, kLocalDismissPet}, players, result));
        finishCast();
        world.tick(0.05f, players);
        SELFTEST_CHECK(!livePet() && !p.hunterPet.active && p.hunterPet.entry == 2031);
        p.globalCooldownMs = 0;
        SELFTEST_CHECK(world.execute(p, {LocalAction::CastSpell, 0, kLocalCallPet}, players, result));
        world.tick(0.05f, players);
        SELFTEST_CHECK(livePet() && p.hunterPet.active);
        // Out of the world and back (a travel retire): it returns on its own.
        p.flight.active = true; world.tick(0.05f, players); p.flight.active = false;
        for (int i = 0; i < 4; ++i) world.tick(0.05f, players);
        SELFTEST_CHECK(livePet() && livePet()->entry == 2031);
        // The pet fights beside its hunter: the owner attacks a beast, the pet
        // joins, damages it and spends focus on its basic attack. Nearest
        // living target first; up to three targets (one may evade or be taken).
        {
            bool petEngaged = false, boarHurt = false, focusSpent = false;
            std::set<uint64_t> tried;
            for (int round = 0; round < 3 && !(petEngaged && boarHurt && focusSpent); ++round) {
                const LocalRealmNpc* boar = nullptr; float bestD = 1e30f;
                for (int i = 0; i < 200 && !boar; ++i) { world.tick(0.05f, players);
                    for (const auto& n : world.npcs()) if ((n.entry == 1984 || n.entry == 2031) && !n.dead && !tried.count(n.guid)) {
                        const float d = std::hypot(n.x - p.x, n.y - p.y); if (d < bestD && d < 80) { bestD = d; boar = &n; } } }
                if (!boar) break;
                const uint64_t boarGuid = boar->guid; const uint32_t before = boar->maxHealth; tried.insert(boarGuid);
                p.x = boar->x + 1; p.y = boar->y; p.z = boar->z; ++p.positionRevision;
                world.execute(p, {LocalAction::Attack, boarGuid, 0}, players, result);
                petEngaged = boarHurt = false;
                for (int i = 0; i < 600 && !(petEngaged && boarHurt && focusSpent); ++i) {
                    p.health = p.maxHealth; world.tick(0.05f, players);
                    if (const auto* v = livePet(); v && v->targetGuid == boarGuid) petEngaged = true;
                    if (const auto* v = livePet(); v && v->power < v->maxPower) focusSpent = true;
                    for (const auto& n : world.npcs()) if (n.guid == boarGuid && (n.dead || n.health < before)) boarHurt = true;
                }
            }
            SELFTEST_CHECK(petEngaged && boarHurt);
            SELFTEST_CHECK(focusSpent); // Claw/Bite/Smack spends 25 focus.
            SELFTEST_CHECK(c.petFamilyAttack[2] == 2 && c.petFamilyAttack[1] == 1 && c.petBasicRanks[1].size() == 11);
        }
        out << "PASS hunter pets: " << c.tameableBeasts.size() << " tameable beasts; tame Young Nightsaber (20 s channel, broken by moving), one-pet rule, dismiss, call, return after travel, fights beside the hunter with its family's basic attack\n";
    }

    // ---- 2c. Class abilities from the client's own Spell.dbc, cast in combat.
    if (!clientSpells) out << "SKIP class abilities (no client spell import)\n";
    else {
        const uint32_t bow = findItem(2, 2, 15, c), sword2h = findItem(2, 8, 17, c);
        uint32_t arrows = 0;
        for (const auto& m : kLocalAuctionItems) if (m.itemClass == 6 && m.subClass == 2 && m.requiredLevel <= 1 && c.item(m.id)) { arrows = m.id; break; }
        SELFTEST_CHECK(bow && sword2h && arrows);
        // kind: 0 damages the enemy, 1 lands (interrupt, taunt), 2 creates an
        // item, 3 raises the caster's stats, 4 mounts the caster, 5 teleports it.
        struct Ability { uint8_t race, cls; const char* name; int kind; };
        const Ability abilities[] = {
            {1, 1, "Mortal Strike", false}, {1, 1, "Heroic Strike", false}, {1, 1, "Overpower", false}, {1, 1, "Pummel", true},
            {4, 3, "Arcane Shot", false}, {4, 3, "Aimed Shot", false}, {4, 3, "Raptor Strike", false},
            {1, 4, "Kick", true}, {1, 6, "Icy Touch", false}, {1, 6, "Plague Strike", false}, {1, 6, "Blood Strike", false},
            {1, 6, "Mind Freeze", true}, {1, 8, "Counterspell", true}, {1, 8, "Frostfire Bolt", false}, {11, 7, "Earth Shock", false},
            {1, 9, "Haunt", false}, {4, 3, "Serpent Sting", 0}, {1, 1, "Taunt", 1}, {1, 8, "Conjure Water", 2},
            {1, 5, "Power Word: Fortitude", 3}, {1, 1, "Battle Shout", 3}, {1, 9, "Felsteed", 4},
            {1, 8, "Arcane Brilliance", 3}, {4, 11, "Gift of the Wild", 3},
            {1, 9, "Drain Life", 0}, {1, 8, "Arcane Missiles", 0}, {1, 5, "Mind Flay", 0}, {1, 8, "Teleport: Stormwind", 5},
            {1, 8, "Blizzard", 0}, {1, 2, "Consecration", 0}, {1, 9, "Rain of Fire", 0},
        };
        size_t passed = 0;
        for (const auto& a : abilities) {
            LocalGameplay arena; SELFTEST_CHECK(arena.loadContent(worldPath, error));
            SELFTEST_CHECK(arena.setStarterSpells(*clientSpells, "selftest", error));
            LocalRealmPlayer p; p.guid = 500 + passed; p.race = a.race; p.classId = a.cls; p.name = "Tester";
            arena.initializePlayer(p, true, 80);
            // Ebon Hold's creatures are scripted, not fair game: test a death
            // knight in Northshire like everyone else's first enemies.
            if (a.cls == 6) for (const auto& start : c.catalog->starts()) if (start.race == 1 && start.classId == 1) {
                p.mapId = start.mapId; p.x = start.x; p.y = start.y; p.z = start.z; ++p.positionRevision; break; }
            std::vector<LocalRealmPlayer*> players{&p};
            const auto& content = arena.content();
            uint32_t spellId = 0;
            for (auto id : p.knownSpells) if (const auto* d = content.spell(id); d && d->name == a.name && d->unsupportedReason.empty()) spellId = id;
            // Mounts come from the class trainer, not the level progression.
            if (!spellId && a.kind == 4) for (const auto& d : content.spells)
                if (d.name == a.name && d.mountDisplayId && (d.allowableClasses & (1u << (a.cls - 1)))) { spellId = d.id; p.knownSpells.push_back(d.id); break; }
            if (!spellId) { out << "FAIL class ability " << a.name << ": not in the level 80 spellbook\n"; return false; }
            // Weapons the ability needs.
            p.inventory.push_back({a.cls == 3 ? bow : sword2h, 1, 20});
            if (a.cls == 3) { p.inventory.push_back({sword2h, 1, 21}); p.inventory.push_back({arrows, 200, 22}); }
            if (a.cls == 7) p.inventory.push_back({findItem(2, 4, 13, c), 1, 23});
            normalizeLocalInventory(p);
            for (const auto& stack : std::vector<LocalItemStack>(p.inventory))
                if (const auto* m = localAuctionMetadata(stack.itemId); m && m->itemClass == 2)
                    arena.execute(p, {LocalAction::EquipItem, 0, stack.itemId}, players, result);
            // Reagents: refused without them, consumed by the cast.
            if (const auto* sd = content.spell(spellId); sd && sd->reagentItems[0]) {
                p.globalCooldownMs = 0; p.mana = p.maxMana;
                if (arena.execute(p, {LocalAction::CastSpell, p.guid, spellId}, players, result) || result.find("reagent") == std::string::npos) {
                    out << "FAIL class ability " << a.name << ": cast without its reagent (" << result << ")\n"; return false; }
                for (size_t r = 0; r < sd->reagentItems.size(); ++r) if (sd->reagentItems[r]) p.inventory.push_back({sd->reagentItems[r], uint16_t(sd->reagentCounts[r] * 2), uint8_t(10 + r)});
                normalizeLocalInventory(p);
            }
            const uint32_t reagentBefore = [&] { const auto* sd = content.spell(spellId); uint32_t n = 0;
                for (const auto& st : p.inventory) if (sd && st.itemId == sd->reagentItems[0]) n += st.count; return n; }();
            if (a.kind >= 2) {
                const auto meleeBefore = localMeleeStats(p, content); const auto healthBefore = p.maxHealth; const auto items = p.inventory.size();
                p.ridingSkill = 150; p.mana = p.maxMana; p.globalCooldownMs = 0;
                bool ok = arena.execute(p, {LocalAction::CastSpell, p.guid, spellId}, players, result);
                for (int t = 0; t < 200 && p.castingSpellId; ++t) arena.tick(0.05f, players);
                arena.tick(0.05f, players);
                const auto meleeAfter = localMeleeStats(p, content);
                if (a.kind == 2) ok = ok && p.inventory.size() > items;
                if (a.kind == 3) ok = ok && (meleeAfter.attackPower > meleeBefore.attackPower || p.maxHealth > healthBefore ||
                                             meleeAfter.attributes[3] > meleeBefore.attributes[3]);
                if (a.kind == 4) ok = ok && p.mountSpellId == spellId;
                if (a.kind == 5) { const auto* to = content.spellDestination(spellId);
                    ok = ok && to && p.mapId == to->mapId && std::hypot(p.x - to->x, p.y - to->y) < 5; }
                if (const auto* sd = content.spell(spellId); ok && sd && sd->reagentItems[0]) {
                    uint32_t n = 0; for (const auto& st : p.inventory) if (st.itemId == sd->reagentItems[0]) n += st.count;
                    ok = n + sd->reagentCounts[0] == reagentBefore;
                    if (!ok) result = "reagent not consumed";
                }
                if (!ok) { out << "FAIL class ability " << a.name << ": " << result << "\n"; return false; }
                ++passed; continue;
            }
            // A living enemy beside the character, faced.
            std::vector<uint64_t> foes;
            for (int i = 0; i < 200 && foes.empty(); ++i) { arena.tick(0.05f, players);
                for (const auto& n : arena.npcs()) if (n.hostile && !n.dead && n.health && std::hypot(n.x - p.x, n.y - p.y) < 120) foes.push_back(n.guid); }
            if (foes.empty()) { out << "FAIL class ability " << a.name << ": no enemy near the start\n"; return false; }
            uint64_t foeGuid = foes.front(); size_t foeIndex = 0;
            bool landed = false; std::string last;
            for (int attempt = 0; attempt < 12 && !landed; ++attempt) {
                // Some "hostile" creatures at a start (Ebon Hold) cannot be attacked: try the next one.
                if (last.find("living enemy") != std::string::npos && foeIndex + 1 < foes.size()) { foeGuid = foes[++foeIndex]; last.clear(); }
                const LocalRealmNpc* n = nullptr;
                for (const auto& v : arena.npcs()) if (v.guid == foeGuid) n = &v;
                if (!n || n->dead) break;
                const bool ranged = a.cls == 3 && std::string(a.name) != "Raptor Strike";
                const float gap = ranged ? 15.f : 1.5f;
                p.x = n->x - gap; p.y = n->y; p.z = n->z; p.orientation = 0; ++p.positionRevision;
                p.mana = p.maxMana; p.runeCooldownMs.fill(0); p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear();
                p.health = p.maxHealth;
                const uint32_t before = n->health;
                if (!arena.execute(p, {LocalAction::CastSpell, foeGuid, spellId}, players, result)) {
                    last = result;
                    // A stance-bound ability (Overpower): take the next known stance and retry.
                    if (result.find("form or stance") != std::string::npos) {
                        std::vector<uint32_t> forms;
                        for (auto id : p.knownSpells) if (const auto* f = content.spell(id); f && f->formId && f->unsupportedReason.empty()) forms.push_back(id);
                        if (!forms.empty()) {
                            p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear();
                            arena.execute(p, {LocalAction::CastSpell, p.guid, forms[size_t(attempt) % forms.size()]}, players, result);
                        }
                    }
                    arena.tick(0.05f, players); continue;
                }
                for (int t = 0; t < 120 && p.castingSpellId; ++t) { p.health = p.maxHealth; arena.tick(0.05f, players); }
                arena.tick(0.05f, players);
                const LocalRealmNpc* after = nullptr;
                for (const auto& v : arena.npcs()) if (v.guid == foeGuid) after = &v;
                if (a.kind == 1) landed = p.lastCastSpellId == spellId;
                else if (a.kind == 0) {
                    // A damage-over-time spell lands its first tick a few seconds later.
                    for (int t = 0; t < 90 && after && !after->dead && after->health >= before; ++t) {
                        p.health = p.maxHealth; arena.tick(0.05f, players); after = nullptr;
                        for (const auto& v : arena.npcs()) if (v.guid == foeGuid) after = &v;
                    }
                    landed = !after || after->dead || after->health < before;
                }
                last = result;
            }
            if (!landed) { out << "FAIL class ability " << a.name << ": " << last << "\n"; return false; }
            ++passed;
        }
        out << "PASS class abilities: " << passed << " Spell.dbc abilities (weapon strikes, shots, DoTs, channels, ground areas, interrupts, taunts, spells, conjuring, stat buffs, reagents, class mounts, teleports)\n";
    }

    // ---- 2d. Every chain is reachable: closure over the realm's own gates.
    {
        std::set<uint32_t> reached; bool grew = true;
        size_t blocked = 0;
        for (const auto& [id, gate] : c.questChainGates) if (!gate.unsupportedReason.empty()) ++blocked;
        while (grew) {
            grew = false;
            LocalRealmPlayer probe; probe.race = 1; probe.classId = 1; probe.level = 1;
            probe.completedQuestIds.assign(reached.begin(), reached.end());
            for (const auto& [id, gate] : c.questChainGates) {
                if (reached.count(id)) continue;
                const auto* q = c.quest(id); if (!q) continue;
                // Item, reputation and spell conditions are things a player can
                // go and get; the question here is the quest structure alone.
                auto structural = *q;
                // Exclusive choices ("not having done X") are a player's pick, not structure.
                for (auto& alternative : structural.chainGate.alternatives) {
                    alternative.player.clear();
                    std::erase_if(alternative.quests, [](const auto& predicate) { return predicate.statusMask == LocalQuestChainNone; });
                }
                if (localQuestChainSatisfied(probe, structural)) { reached.insert(id); grew = true; }
            }
        }
        const size_t total = c.questChainGates.size();
        if (std::getenv("QUEST_VERBOSE")) for (const auto& [id, gate] : c.questChainGates) if (!reached.count(id))
            if (const auto* q = c.quest(id)) out << "  unreachable " << id << " '" << q->title << "' " << gate.unsupportedReason << "\n";
        out << "QUEST CHAINS reachable=" << reached.size() << "/" << total << " blocked=" << blocked << "\n";
        SELFTEST_CHECK(reached.size() + blocked >= total - 3);
        out << "PASS quest chains: " << reached.size() << " of " << total << " quests reachable from a fresh character\n";
    }

    if (!quests) { out << "SKIP start-zone quests\n"; return true; }
    // ---- 3. Start-zone quests from every start, driven through the real commands.
    std::map<std::pair<uint8_t, uint8_t>, size_t> rewarded;
    std::set<uint32_t> everRewarded, everAccepted;
    size_t startIndex = 0;
    std::vector<std::string> failures;
    const char* only = std::getenv("QUEST_ONLY"); // "race:class" to debug one start
    const bool verbose = std::getenv("QUEST_VERBOSE") != nullptr;
    for (const auto& start : c.catalog->starts()) {
        if (only && std::to_string(start.race) + ":" + std::to_string(start.classId) != only) continue;
        LocalGameplay world; SELFTEST_CHECK(world.loadContent(worldPath, error));
        LocalRealmPlayer p; p.guid = 1000 + startIndex++; p.race = start.race; p.classId = start.classId; p.name = "Quester";
        world.initializePlayer(p, true, 0);
        std::vector<LocalRealmPlayer*> players{&p};
        const float homeX = p.x, homeY = p.y; const uint32_t homeMap = p.mapId;
        const auto moveTo = [&](uint32_t entry) -> const LocalRealmNpc* {
            // Catalog spawns of the entry nearest the start first; a spawn
            // whose creature is dead (respawning) moves on to the next one.
            std::vector<std::pair<float, const Spawn*>> candidates;
            for (auto [it, end] = spawns.equal_range(entry); it != end; ++it) if (it->second.map == homeMap) {
                const float d = std::hypot(it->second.x - homeX, it->second.y - homeY); if (d <= 3000) candidates.emplace_back(d, &it->second); }
            std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            if (candidates.size() > 24) candidates.resize(24);
            for (const auto& [distance, spawn] : candidates) {
                p.x = spawn->x; p.y = spawn->y; p.z = spawn->z; ++p.positionRevision;
                const LocalRealmNpc* found = nullptr; float fd = 1e30f;
                for (int i = 0; i < 100 && !found; ++i) {
                    world.tick(0.05f, players);
                    for (const auto& n : world.npcs()) if (n.entry == entry && !n.dead) { const float d = std::hypot(n.x - p.x, n.y - p.y); if (d < fd && d < 120) { fd = d; found = &n; } }
                }
                if (found) { p.x = found->x; p.y = found->y; p.z = found->z; ++p.positionRevision; return found; }
            }
            return nullptr;
        };
        std::set<uint32_t> tried;
        for (int round = 0; round < 12; ++round) {
            // Quests offered near the start that this character may take now.
            const LocalQuestDefinition* pick = nullptr;
            for (const auto& [id, gate] : c.questChainGates) {
                const auto* q = c.quest(id); if (!q || tried.count(id) || q->minLevel > p.level + 2) continue;
                if (localQuestAcceptanceError(p, *q)) continue;
                const Spawn* g = nullptr;
                for (auto [it, end] = spawns.equal_range(q->giverEntry); it != end; ++it)
                    if (it->second.map == homeMap && std::hypot(it->second.x - homeX, it->second.y - homeY) < 400) { g = &it->second; break; }
                if (!g) continue;
                pick = q; break;
            }
            if (!pick) break;
            tried.insert(pick->id);
            const auto label = std::string(kClassNames[p.classId]) + " race " + std::to_string(p.race) + " quest " + std::to_string(pick->id) + " '" + pick->title + "'";
            const auto* giver = moveTo(pick->giverEntry);
            if (!giver) { failures.push_back(label + ": giver " + std::to_string(pick->giverEntry) + " not streamed in"); continue; }
            if (!world.execute(p, {LocalAction::AcceptQuest, giver->guid, pick->id}, players, result)) {
                failures.push_back(label + ": accept failed: " + result); continue; }
            everAccepted.insert(pick->id);
            for (const auto& o : pick->objectives) {
                for (int guard = 0; guard < 80; ++guard) {
                    const auto it = std::find_if(p.quests.begin(), p.quests.end(), [&](const auto& q) { return q.id == pick->id; });
                    if (it == p.quests.end() || it->status == LocalQuestStatus::Complete) break;
                    const size_t index = size_t(&o - &pick->objectives[0]);
                    if (it->progress[index] >= o.count) break;
                    if (o.type == LocalQuestObjective::Type::Talk) {
                        const auto* t = moveTo(o.entry); if (!t) break;
                        world.execute(p, {LocalAction::Interact, t->guid, 0}, players, result);
                        if (guard > 2) break;
                    } else {
                        uint32_t target = o.entry;
                        if (o.type == LocalQuestObjective::Type::Collect) {
                            target = 0;
                            // Any spawned creature near home whose loot carries the item.
                            float bestD = 1e30f;
                            for (const auto& [entry, s] : spawns) if (s.map == homeMap) {
                                const float d = std::hypot(s.x - homeX, s.y - homeY); if (d > 3000 || d >= bestD) continue;
                                const auto* def = c.npc(entry); if (!def) continue;
                                if (std::any_of(def->loot.begin(), def->loot.end(), [&](const auto& l) { return l.itemId == o.entry; })) { bestD = d; target = entry; }
                            }
                            if (!target) break;
                        }
                        const auto* foe = moveTo(target); if (!foe) { if (verbose) out << "  q" << pick->id << " no spawn of " << target << "\n"; break; }
                        const uint64_t guid = foe->guid;
                        world.execute(p, {LocalAction::Attack, guid, 0}, players, result);
                        for (int t = 0; t < 600; ++t) {
                            p.health = p.maxHealth; world.tick(0.05f, players);
                            const auto* n = std::find_if(world.npcs().begin(), world.npcs().end(), [&](const auto& x) { return x.guid == guid; }) != world.npcs().end()
                                ? &*std::find_if(world.npcs().begin(), world.npcs().end(), [&](const auto& x) { return x.guid == guid; }) : nullptr;
                            if (!n || n->dead) break;
                            if (std::hypot(n->x - p.x, n->y - p.y) > 2) { p.x = n->x; p.y = n->y; p.z = n->z; ++p.positionRevision; }
                            if (!p.attackTarget) world.execute(p, {LocalAction::Attack, guid, 0}, players, result);
                        }
                        if (o.type == LocalQuestObjective::Type::Collect) world.execute(p, {LocalAction::Loot, guid, 0}, players, result);
                        if (verbose) { const auto q = std::find_if(p.quests.begin(), p.quests.end(), [&](const auto& x) { return x.id == pick->id; });
                            out << "  q" << pick->id << " target " << target << " guid " << guid << " -> progress " << (q != p.quests.end() ? q->progress[index] : 999) << "/" << o.count << " (" << result << ")\n"; }
                    }
                }
            }
            const auto it = std::find_if(p.quests.begin(), p.quests.end(), [&](const auto& q) { return q.id == pick->id; });
            if (it == p.quests.end() || it->status != LocalQuestStatus::Complete) {
                failures.push_back(label + ": objectives incomplete"); world.execute(p, {LocalAction::AbandonQuest, 0, pick->id}, players, result); continue; }
            // Drop loot no objective needs so the reward always has room (the
            // simulation loots every creature a collect objective visits).
            std::erase_if(p.inventory, [&](const auto& stack) {
                if (std::count(p.equipment.begin(), p.equipment.end(), stack.itemId)) return false;
                for (const auto& q : p.quests) if (const auto* d = c.quest(q.id))
                    for (const auto& o : d->objectives) if (o.type == LocalQuestObjective::Type::Collect && o.entry == stack.itemId) return false;
                return !c.consumable(stack.itemId);
            });
            const auto* ender = moveTo(pick->turnInEntry);
            const uint64_t choice = pick->rewardChoices.empty() ? 0 : 1;
            LocalRealmCommand turnIn{LocalAction::TurnInQuest, ender ? ender->guid : 0, pick->id}; turnIn.bid = choice;
            if (!ender) { failures.push_back(label + ": turn-in npc " + std::to_string(pick->turnInEntry) + " not streamed in"); continue; }
            if (!world.execute(p, turnIn, players, result)) { failures.push_back(label + ": turn-in failed: " + result); continue; }
            ++rewarded[{p.race, p.classId}]; everRewarded.insert(pick->id);
        }
        p.x = homeX; p.y = homeY;
        out << "progress start " << startIndex << "/" << c.catalog->starts().size() << " race " << unsigned(p.race) << " " << kClassNames[p.classId]
            << " rewarded " << rewarded[{p.race, p.classId}] << "\n";
    }
    size_t startsWithQuest = 0; for (const auto& [k, n] : rewarded) if (n) ++startsWithQuest;
    out << "QUESTS starts=" << startIndex << " startsWithRewardedQuest=" << startsWithQuest
              << " distinctAccepted=" << everAccepted.size() << " distinctRewarded=" << everRewarded.size() << " failures=" << failures.size() << "\n";
    for (const auto& [k, n] : rewarded) out << "  race " << unsigned(k.first) << " " << kClassNames[k.second] << ": " << n << " quests rewarded\n";
    std::set<std::string> uniq(failures.begin(), failures.end());
    for (const auto& f : uniq) out << "  FAILURE " << f << "\n";
    if (startsWithQuest != startIndex) { out << "FAIL some starts completed no quest\n"; return false; }
    out << "PASS start-zone quests: every start accepted, completed and turned in quests\n";
    return true;
}
#undef SELFTEST_CHECK
} // namespace wowee::game
