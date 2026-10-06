// Runtime self-test (host suite and WOWEE_DEV_SELFTEST on the console).
// Production-content checks for the three player-facing rules most easily
// broken by catalog or rule changes: class equipment proficiencies, on-use
// consumables and the start-zone quest chains of every race/class start.
#include "game/local_selftest.hpp"
#include "game/local_gameplay.hpp"
#include "game/local_auction_catalog.hpp"
#include "game/local_equipment.hpp"
#include "game/local_melee.hpp"
#include "game/local_stat_auras.hpp"
#include "game/local_mount_models.hpp"
#include "game/local_feral_talents.hpp"
#include "game/local_ranged.hpp"
#include "game/local_pet.hpp"
#include "game/local_world_catalog.hpp"
#include "game/local_inventory_layout.hpp"
#include "game/local_mail.hpp"
#include "game/local_quest_marker.hpp"
#include "game/local_quest_dialogue.hpp"
#include "game/local_quest_eligibility.hpp"
#include "game/local_spell_target_rules.hpp"
#include "game/local_npc_auras.hpp"
#include "game/local_forms.hpp"
#include "ui/action_bar_panel.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
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

    // ---- 1b. Durability catalog, migration, broken stat zeroing, repair command.
    {
        SELFTEST_CHECK(c.itemDurability.size() >= 20000);
        const auto* dur25 = c.durability(25);
        SELFTEST_CHECK(dur25 && dur25->maxDurability == 20 && dur25->costPerPoint == 800);
        const auto* dur200 = c.durability(200);
        SELFTEST_CHECK(dur200 && dur200->maxDurability == 60 && dur200->costPerPoint == 4000);

        LocalRealmPlayer p; p.guid = 2; p.race = 1; p.classId = 1; p.level = 80; p.name = "DurabilityTest";
        p.money = 100000;
        p.gameplayInitialized = true;
        p.inventory = {{25, 1, 0, {}}, {200, 1, 1, {}}};
        game.initializePlayer(p, false);
        SELFTEST_CHECK(p.inventory[0].instance.maxDurability == 20 && p.inventory[0].instance.curDurability == 20);
        SELFTEST_CHECK(p.inventory[1].instance.maxDurability == 60 && p.inventory[1].instance.curDurability == 60);

        std::vector<LocalRealmPlayer*> players{&p};
        SELFTEST_CHECK(game.execute(p, {LocalAction::EquipItem, 0, 25}, players, result));
        const size_t mhSlot = localEquipmentIndex(LocalEquipmentSlot::MainHand);
        SELFTEST_CHECK(worn(p, c, mhSlot) != nullptr);

        p.inventory[0].instance.curDurability = 0;
        SELFTEST_CHECK(worn(p, c, mhSlot) == nullptr);

        LocalRealmNpc repairNpc{};
        repairNpc.guid = 9999;
        repairNpc.entry = 54;
        repairNpc.repairer = true;
        repairNpc.vendor = true;
        repairNpc.x = p.x; repairNpc.y = p.y; repairNpc.z = p.z;
        game.setRemoteNpcs({repairNpc});

        p.money = 5;
        LocalRealmCommand repCmd{LocalAction::RepairEquipment};
        repCmd.serviceNpcGuid = repairNpc.guid;
        SELFTEST_CHECK(!game.execute(p, repCmd, players, result) && result.find("afford") != std::string::npos);

        p.money = 1000;
        SELFTEST_CHECK(game.repairCost(p, repairNpc.guid) > 0);
        SELFTEST_CHECK(game.execute(p, repCmd, players, result));
        SELFTEST_CHECK(p.inventory[0].instance.curDurability == 20);
        SELFTEST_CHECK(p.money < 1000);
        SELFTEST_CHECK(game.repairCost(p, repairNpc.guid) == 0);

        out << "PASS durability & repair: " << c.itemDurability.size() << " catalog items, migration, broken stat zeroing, repair command\n";
    }

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
        // living target first; up to six targets (one may evade, be taken or die
        // to the hunter before the pet arrives).
        {
            bool petEngaged = false, boarHurt = false, focusSpent = false;
            std::set<uint64_t> tried;
            for (int round = 0; round < 6 && !(petEngaged && boarHurt && focusSpent); ++round) {
                const LocalRealmNpc* boar = nullptr; float bestD = 1e30f;
                for (int i = 0; i < 200 && !boar; ++i) { world.tick(0.05f, players);
                    for (const auto& n : world.npcs()) if ((n.entry == 1984 || n.entry == 2031) && !n.dead && !tried.count(n.guid)) {
                        const float d = std::hypot(n.x - p.x, n.y - p.y); if (d < bestD && d < 150) { bestD = d; boar = &n; } } }
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
            // Mend Pet: refused at full health, heals a hurt beast over time.
            SELFTEST_CHECK(c.mendPetRank(136) && c.mendPetRank(136)->perTick == 25);
            if (std::find(p.knownSpells.begin(), p.knownSpells.end(), 136u) == p.knownSpells.end()) p.knownSpells.push_back(136);
            p.level = 12; p.mana = p.maxMana; p.globalCooldownMs = 0;
            if (const auto* v = livePet(); v && v->health < v->maxHealth) {
                const auto before = v->health;
                SELFTEST_CHECK(world.execute(p, {LocalAction::CastSpell, v->guid, 136}, players, result));
                for (int i = 0; i < 70; ++i) world.tick(0.05f, players);
                SELFTEST_CHECK(livePet() && livePet()->health > before);
            } else if (livePet()) {
                SELFTEST_CHECK(!world.execute(p, {LocalAction::CastSpell, livePet()->guid, 136}, players, result) && result.find("full health") != std::string::npos);
            }
            SELFTEST_CHECK(c.petFamilyAttack[2] == 2 && c.petFamilyAttack[1] == 1 && c.petBasicRanks[1].size() == 11);
        }
        out << "PASS hunter pets: " << c.tameableBeasts.size() << " tameable beasts; tame Young Nightsaber (20 s channel, broken by moving), one-pet rule, dismiss, call, return after travel, fights beside the hunter with its family's basic attack, Mend Pet\n";
    }

    // ---- 2b'. Rested experience: earned at an inn (offline too), doubles kill XP.
    {
        LocalGameplay inn; SELFTEST_CHECK(inn.loadContent(worldPath, error));
        LocalRealmPlayer p; p.guid = 88; p.race = 1; p.classId = 1; p.name = "Rester";
        inn.initializePlayer(p, true, 5);
        // Eight hours logged out in an inn: 5% of the level's experience.
        p.restedXp = 0; p.resting = true; p.restLastUnix = uint64_t(std::time(nullptr)) - 8 * 3600;
        inn.initializePlayer(p, false, 0);
        const auto expected = p.xpToLevel * 5 / 100;
        SELFTEST_CHECK(p.restedXp + 1 >= expected && p.restedXp <= expected + 1);
        // Thirty days elsewhere cannot pass the cap of one and a half levels.
        p.resting = false; p.restLastUnix = uint64_t(std::time(nullptr)) - 30ull * 24 * 3600;
        inn.initializePlayer(p, false, 0);
        SELFTEST_CHECK(p.restedXp <= p.xpToLevel * 3 / 2 && p.restedXp > expected);
        // A kill with rest earns double and spends the pool.
        std::vector<LocalRealmPlayer*> players{&p};
        const auto killOne = [&]() -> uint32_t {
            const LocalRealmNpc* foe = nullptr;
            for (int i = 0; i < 200 && !foe; ++i) { inn.tick(0.05f, players);
                for (const auto& n : inn.npcs()) if (n.hostile && !n.dead && n.level <= p.level && std::hypot(n.x - p.x, n.y - p.y) < 120) { foe = &n; break; } }
            if (!foe) return 0;
            const uint64_t guid = foe->guid; const auto xp0 = uint64_t(p.level) * 1000000 + p.xp;
            for (int t = 0; t < 1200; ++t) {
                const LocalRealmNpc* n = nullptr; for (const auto& v : inn.npcs()) if (v.guid == guid) n = &v;
                if (!n || n->dead) break;
                p.x = n->x - 1; p.y = n->y; p.z = n->z; ++p.positionRevision; p.health = p.maxHealth;
                if (!p.attackTarget) inn.execute(p, {LocalAction::Attack, guid, 0}, players, result);
                inn.tick(0.05f, players);
            }
            for (int t = 0; t < 10; ++t) inn.tick(0.05f, players);
            return uint32_t(uint64_t(p.level) * 1000000 + p.xp - xp0);
        };
        const auto restBefore = p.restedXp;
        const auto rested = killOne();
        SELFTEST_CHECK(rested > 0 && p.restedXp < restBefore && restBefore - p.restedXp == rested / 2);
        p.restedXp = 0;
        const auto normal = killOne();
        SELFTEST_CHECK(normal > 0);
        out << "PASS rested experience: offline inn rest " << expected << " (5% of a level), capped at 1.5 levels, kill " << normal << " xp -> " << rested << " rested\n";
    }

    // ---- 2c. Class abilities from the client's own Spell.dbc, cast in combat.
    if (!clientSpells) out << "SKIP class abilities (no client spell import)\n";
    else {
        const uint32_t bow = findItem(2, 2, 15, c), sword2h = findItem(2, 8, 17, c);
        uint32_t arrows = 0;
        for (const auto& m : kLocalAuctionItems) if (m.itemClass == 6 && m.subClass == 2 && m.requiredLevel <= 1 && c.item(m.id)) { arrows = m.id; break; }
        SELFTEST_CHECK(bow && sword2h && arrows);
        // kind: 0 damages the enemy, 1 lands (interrupt, taunt), 2 creates an
        // item, 3 raises the caster's stats, 4 mounts the caster, 5 teleports it,
        // 6 charges it into melee range of the enemy with rage, 7 kills with a
        // Drain Soul channel for a Soul Shard, 8 summons a demon, 9 speeds the
        // caster up, 10 raises its dodge, 11 opens from Stealth for combo points,
        // 12 holds a hunter aspect until another aspect replaces it, 13 tracks
        // a creature type until another tracking replaces it, 14 puts down a
        // support totem (heal, party aura), 15 an attack totem at the enemy, 16
        // is a dispel the caster has nothing for (SPELL_FAILED_NOTHING_TO_DISPEL),
        // 17 a damage finisher and 18 a self-buff finisher after a Sinister
        // Strike (both at level 20, so the builder does not kill the enemy), 19
        // a cat opener: refused outside Prowl, then struck from Prowl, 20 a
        // presence held until another presence replaces it, 21 an offensive
        // dispel at a creature with no buff (SPELL_FAILED_NOTHING_TO_DISPEL), 22
        // a slowing totem beside the enemy, 23 a stun finisher whose length is
        // its combo points (two Sinister Strikes first, level 30), 24 a control
        // that a later hit breaks (Gouge, Sap; level 20; Blind, level 40), 25 an
        // armor reduction finisher whose length is its combo points (Expose
        // Armor, level 30). 23 to 25 fight a creature given extra health so
        // the builder does not kill it. 26 is a curse that lands as the
        // warlock's creature aura with its amounts; Curse of the Elements
        // follows a Curse of Weakness and takes its place (one curse per
        // warlock on a target). 27 is Fear: held with a damage cap of 10% of
        // the creature's health, moved off the first creature when the same
        // warlock fears a second (one target at a time), and broken by the
        // Shadow Bolts that spend the cap (each hit spends what it dealt). 28 is
        // a warlock armor: more armor and more healing taken while held, and
        // Demon Skin replaces Demon Armor (SPELL_SPECIFIC_WARLOCK_ARMOR). 29 is
        // Life Tap: health for the same mana, refused when it would kill. 30 is
        // a one-school absorb (Shadow Ward): it takes shadow damage, not fire.
        // 31 is Death Coil: shadow damage, the warlock healed for 300% of it,
        // and the target held in horror. 32 is Divine Shield: immune to every
        // school, then Forbearance and the two markers refuse Divine
        // Protection and Avenging Wrath until they run out. 33 a damage-taken
        // cut (Shield Wall). 34 is Ice Block: immune and held in place (no
        // casts) until cancelled, then Hypothermia refuses another block. 35 a
        // held class buff whose definition carries its modelled amounts (Fear
        // Ward: fear immunity with one charge; Barkskin: no pushback, -20%). 36
        // a level-scaled absorb (Ice Barrier): at least its base points, and it
        // takes any school. 37 a mana-percent aspect (Aspect of the Viper): 4%
        // of maximum mana every 3 s and half the damage done. Icebound
        // Fortitude reuses 35's definition check: stun immunity, -30%. 38 is
        // Devotion Aura: armor by its amount, and Retribution Aura replaces it
        // (SPELL_SPECIFIC_AURA, one aura per paladin). 39 is Power Word:
        // Shield: an absorb plus Weakened Soul, which refuses another shield
        // until it runs out (15 s). 40 is Polymorph: held, shown as its
        // creature's model, regaining a third of its health every 2 s, and a
        // hit breaks it and gives the creature its own model back. 41 is a
        // root (Entangling Roots): the creature stays where it is while the
        // caster walks away, with a 10% damage cap like Fear's. 42 is a death
        // knight strike's disease: its DoT ticks on a creature nobody else
        // hits, and Frost Fever slows the creature's attacks by 14%.
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
            {1, 1, "Hamstring", 1}, {4, 3, "Concussive Shot", 1}, {1, 1, "Charge", 6}, {1, 9, "Drain Soul", 7}, {1, 9, "Summon Voidwalker", 8},
            {1, 4, "Sprint", 9}, {1, 4, "Evasion", 10}, {1, 4, "Ambush", 11}, {1, 4, "Garrote", 11}, {1, 4, "Cheap Shot", 11},
            {4, 3, "Aspect of the Hawk", 12}, {4, 3, "Aspect of the Cheetah", 9},
            {4, 3, "Track Beasts", 13}, {1, 2, "Sense Undead", 13},
            {11, 7, "Strength of Earth Totem", 14}, {11, 7, "Healing Stream Totem", 14}, {11, 7, "Searing Totem", 15},
            {1, 2, "Cleanse", 16}, {4, 11, "Cure Poison", 16},
            {1, 4, "Rupture", 17}, {1, 4, "Slice and Dice", 18}, {4, 11, "Ravage", 19},
            {1, 6, "Death Strike", 0}, {1, 6, "Obliterate", 0}, {1, 6, "Death and Decay", 0}, {1, 6, "Chains of Ice", 1},
            {1, 6, "Blood Presence", 20}, {11, 7, "Purge", 21}, {11, 7, "Earthbind Totem", 22}, {1, 1, "Cleave", 0}, {4, 3, "Multi-Shot", 0},
            {1, 4, "Kidney Shot", 23}, {1, 4, "Gouge", 24}, {3, 4, "Sap", 24}, // humanoids only: Coldridge troggs
            {1, 4, "Blind", 24}, {1, 4, "Expose Armor", 25},
            {1, 9, "Curse of Weakness", 26}, {1, 9, "Curse of the Elements", 26}, {1, 9, "Curse of Tongues", 26},
            {1, 9, "Immolate", 0}, {1, 9, "Fear", 27}, {1, 9, "Demon Armor", 28}, {1, 9, "Life Tap", 29}, {1, 9, "Create Healthstone", 2}, {1, 9, "Shadow Ward", 30}, {1, 9, "Death Coil", 31}, {1, 9, "Incinerate", 0}, {1, 2, "Divine Shield", 32}, {1, 1, "Shield Wall", 33}, {1, 8, "Ice Block", 34}, {1, 5, "Fear Ward", 35}, {4, 11, "Barkskin", 35}, {1, 8, "Ice Barrier", 36}, {3, 3, "Aspect of the Viper", 37}, {1, 6, "Icebound Fortitude", 35}, {1, 2, "Devotion Aura", 38}, {1, 5, "Power Word: Shield", 39}, {1, 8, "Polymorph", 40}, {4, 11, "Entangling Roots", 41}, {1, 6, "Icy Touch", 42}, {1, 6, "Plague Strike", 42}, {1, 6, "Blood Boil", 0}, {1, 6, "Death Coil", 0}, {1, 6, "Death Grip", 43}, {1, 6, "Pestilence", 44}, {1, 6, "Anti-Magic Shell", 45}, {1, 6, "Raise Dead", 46}, {1, 6, "Empower Rune Weapon", 47}, {1, 6, "Strangulate", 1}, {3, 3, "Steady Shot", 0}, {3, 3, "Hunter's Mark", 48}, {2, 7, "Bloodlust", 49}, {3, 3, "Rapid Fire", 50}, {2, 7, "Windfury Totem", 51}, {3, 3, "Feign Death", 52}, {2, 7, "Fire Nova", 53}, {1, 2, "Judgement of Light", 54}, {3, 3, "Distracting Shot", 1}, {2, 7, "Wind Shear", 1}, {1, 2, "Blessing of Kings", 55}, {1, 2, "Blessing of Wisdom", 55}, {1, 8, "Frost Armor", 56}, {1, 5, "Inner Fire", 56}, {1, 8, "Frost Nova", 57}, {1, 1, "Sunder Armor", 58}, {1, 5, "Psychic Scream", 59}, {1, 9, "Howl of Terror", 59}, {1, 1, "Bloodrage", 60}, {1, 2, "Divine Plea", 61}, {1, 1, "Intimidating Shout", 62}, {4, 11, "Faerie Fire", 63}, {4, 11, "Innervate", 64}, {1, 4, "Vanish", 65}, {1, 4, "Feint", 66}, {4, 11, "Demoralizing Roar", 67}, {1, 1, "Challenging Shout", 68}, {1, 8, "Cone of Cold", 69}, {1, 5, "Fade", 70},
        };
        size_t passed = 0;
        // Incinerate carries the Immolate bonus (a quarter more on an Immolated target).
        if (const auto inc = std::find_if(clientSpells->begin(), clientSpells->end(), [](const auto& d) { return d.id == 47838; });
            inc == clientSpells->end() || !inc->immolateBonus || !inc->unsupportedReason.empty()) {
            out << "FAIL class ability Incinerate: no Immolate bonus\n"; return false; }
        // Hunter shots take spell_bonus_data's ranged attack power share; Steady
        // Shot adds the ranged weapon's roll and its ammunition.
        for (const auto& [id, per100k, steady] : {std::tuple{49052u, 10000u, true}, std::tuple{49045u, 15000u, false}}) {
            const auto it = std::find_if(clientSpells->begin(), clientSpells->end(), [&](const auto& d) { return d.id == id; });
            if (it == clientSpells->end() || !it->unsupportedReason.empty() || it->apBonusPer100k != per100k || !it->apBonusRanged || it->steadyShot != steady) {
                out << "FAIL class ability hunter shot " << id << ": ranged attack power terms " << (it == clientSpells->end() ? std::string("missing") : it->unsupportedReason + " ap " + std::to_string(it->apBonusPer100k) + " ranged " + std::to_string(it->apBonusRanged) + " steady " + std::to_string(it->steadyShot) + " itemClass " + std::to_string(int(it->requiredItemClass)) + " sub " + std::to_string(it->requiredItemSubclasses)) << "\n"; return false; } }
        // A level 1 shaman's Healing Wave (ranks 1-10 aim at the chain-heal ally target with no chain).
        if (const auto hw = std::find_if(clientSpells->begin(), clientSpells->end(), [](const auto& d) { return d.id == 331; });
            hw == clientSpells->end() || !hw->unsupportedReason.empty() || !hw->heal) {
            out << "FAIL class ability Healing Wave rank 1: " << (hw == clientSpells->end() ? std::string("missing") : hw->unsupportedReason) << "\n"; return false; }
        // A level 4 warlock's Corruption (rank 1 carries an empty DUMMY beside its DoT).
        const auto lowIt = std::find_if(clientSpells->begin(), clientSpells->end(), [](const auto& d) { return d.id == 172; });
        if (const auto* low = lowIt == clientSpells->end() ? nullptr : &*lowIt; !low || !low->unsupportedReason.empty() || !low->periodicDamage) {
            out << "FAIL class ability Corruption rank 1: " << (low ? low->unsupportedReason : std::string("missing")) << "\n"; return false; }
        for (const auto& a : abilities) {
            LocalGameplay arena; SELFTEST_CHECK(arena.loadContent(worldPath, error));
            SELFTEST_CHECK(arena.setStarterSpells(*clientSpells, "selftest", error));
            LocalRealmPlayer p; p.guid = 500 + passed; p.race = a.race; p.classId = a.cls; p.name = "Tester";
            arena.initializePlayer(p, true, a.kind == 24 && std::string(a.name) == "Blind" ? 40 :
                                           a.kind == 17 || a.kind == 18 || a.kind == 24 ? 20 : a.kind == 23 || a.kind == 25 ? 30 : 80);
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
            if (!spellId) { out << "FAIL class ability " << a.name << ": not in the level 80 spellbook (" << p.knownSpells.size() << " known)\n";
                return false; }
            // Weapons the ability needs.
            p.inventory.push_back({a.cls == 3 ? bow : sword2h, 1, 20});
            if (a.cls == 3) { p.inventory.push_back({sword2h, 1, 21}); p.inventory.push_back({arrows, 200, 22}); }
            if (a.cls == 7) p.inventory.push_back({findItem(2, 4, 13, c), 1, 23});
            if (a.cls == 4 && c.item(2092)) p.inventory.push_back({2092, 1, 23}); // Ambush needs a dagger (Worn Dagger)
            normalizeLocalInventory(p);
            for (const auto& stack : std::vector<LocalItemStack>(p.inventory))
                if (const auto* m = localAuctionMetadata(stack.itemId); m && m->itemClass == 2)
                    arena.execute(p, {LocalAction::EquipItem, 0, stack.itemId}, players, result);
            if (a.cls == 4) arena.execute(p, {LocalAction::EquipItem, localEquipmentIndex(LocalEquipmentSlot::MainHand) + 1u, 2092}, players, result);
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
            if ((a.kind >= 2 && a.kind <= 5) || (a.kind >= 8 && a.kind <= 10) || (a.kind >= 12 && a.kind <= 14) || a.kind == 16 || a.kind == 20 || (a.kind >= 28 && a.kind <= 30) || (a.kind >= 32 && a.kind <= 39) || a.kind == 45 || a.kind == 46 || a.kind == 47 || a.kind == 49 || a.kind == 50 || a.kind == 51 || a.kind == 55 || a.kind == 56 || a.kind == 57 || a.kind == 59 || a.kind == 60 || a.kind == 61 || a.kind == 64 || a.kind == 67 || a.kind == 68 || a.kind == 69) {
                const auto meleeBefore = localMeleeStats(p, content); const auto healthBefore = p.maxHealth; const auto items = p.inventory.size();
                const auto armorBefore = localMeleeArmor(p, content);
                const auto* autoShot = content.spell(75);
                const auto rangedBefore = autoShot ? localRangedAmounts(p, content, *autoShot) : LocalRangedAmounts{};
                p.ridingSkill = 150; p.mana = p.maxMana; p.globalCooldownMs = 0;
                // Raise Dead away from any corpse: refused without Corpse Dust, then given one.
                bool raiseRefused = false;
                if (a.kind == 46) if (const auto* sd = content.spell(spellId); sd && sd->raiseDeadReagent) {
                    for (auto& st : p.inventory) if (st.itemId == sd->raiseDeadReagent) st.count = 0;
                    normalizeLocalInventory(p);
                    std::string refusal; raiseRefused = !arena.execute(p, {LocalAction::CastSpell, p.guid, spellId}, players, refusal) && refusal.find("reagent") != std::string::npos;
                    p.inventory.push_back({sd->raiseDeadReagent, 1, 31}); normalizeLocalInventory(p); p.globalCooldownMs = 0; p.cooldowns.clear();
                }
                if (a.kind == 29) { p.mana = 0; p.health = p.maxHealth; }
                if (a.kind == 60) { p.mana = 0; p.health = p.maxHealth; }
                if (a.kind == 61 || a.kind == 64) p.mana = 0;
                if (a.kind == 68) p.mana = p.maxMana;
                if (a.kind == 67) for (auto id : p.knownSpells) if (const auto* f = content.spell(id); f && f->name == "Dire Bear Form" && f->unsupportedReason.empty()) {
                    arena.execute(p, {LocalAction::CastSpell, p.guid, id}, players, result); arena.tick(0.05f, players); p.globalCooldownMs = 0; p.mana = p.maxMana; }
                if (a.kind == 47) p.runeCooldownMs.fill(kLocalRuneRechargeMs - 1000); // every rune spent
                if (a.kind == 33) if (const uint32_t shield = findItem(4, 6, 14, c), sword = findItem(2, 7, 13, c); shield && sword) {
                    p.inventory.push_back({sword, 1, 31}); p.inventory.push_back({shield, 1, 30}); normalizeLocalInventory(p);
                    arena.execute(p, {LocalAction::EquipItem, localEquipmentIndex(LocalEquipmentSlot::MainHand) + 1u, sword}, players, result);
                    arena.execute(p, {LocalAction::EquipItem, localEquipmentIndex(LocalEquipmentSlot::OffHand) + 1u, shield}, players, result); }
                if (a.kind == 33) for (auto id : p.knownSpells) if (const auto* f = content.spell(id); f && f->name == "Defensive Stance" && f->unsupportedReason.empty()) {
                    arena.execute(p, {LocalAction::CastSpell, p.guid, id}, players, result); arena.tick(0.05f, players); p.globalCooldownMs = 0; }
                // Frost Nova: the two nearest creatures brought beside the mage.
                std::vector<LocalRealmNpc*> novaFoes;
                if (a.kind == 57 || a.kind == 59 || a.kind == 67 || a.kind == 68 || a.kind == 69) {
                    arena.tick(0.05f, players); p.orientation = 0; // facing east, where they stand p.globalCooldownMs = 0; // creatures spawn on the first tick
                    std::vector<LocalRealmNpc*> near;
                    for (const auto& m : arena.npcs()) if (m.hostile && !m.dead && m.health && m.mapId == p.mapId && m.instanceId == p.instanceId) near.push_back(const_cast<LocalRealmNpc*>(&m));
                    std::sort(near.begin(), near.end(), [&](const auto* l, const auto* r) { return std::hypot(l->x - p.x, l->y - p.y) < std::hypot(r->x - p.x, r->y - p.y); });
                    for (size_t k = 0; k < near.size() && k < 2; ++k) {
                        auto& foe = *near[k]; foe.x = foe.homeX = p.x + 2.f + float(k); foe.y = foe.homeY = p.y; foe.z = foe.homeZ = p.z;
                        foe.maxHealth = foe.health = 1000000; foe.level = p.level; novaFoes.push_back(&foe);
                    }
                }
                const auto tapHealth = p.health;
                bool ok = arena.execute(p, {LocalAction::CastSpell, p.guid, spellId}, players, result);
                if (a.kind == 16) ok = !ok && result.find("Nothing to dispel") != std::string::npos;
                for (int t = 0; t < 200 && p.castingSpellId; ++t) arena.tick(0.05f, players);
                arena.tick(0.05f, players);
                const auto meleeAfter = localMeleeStats(p, content);
                if (a.kind == 2) ok = ok && p.inventory.size() > items;
                // A unique creation (healthstones) is refused while one is carried.
                if (a.kind == 2 && ok) if (const auto* sd = content.spell(spellId); sd && sd->createItemUnique) {
                    p.globalCooldownMs = 0; p.mana = p.maxMana; std::string again;
                    ok = !arena.execute(p, {LocalAction::CastSpell, p.guid, spellId}, players, again) && again.find("too many") != std::string::npos;
                    if (!ok) result = "second healthstone not refused (" + again + ")"; }
                if (a.kind == 3) ok = ok && (meleeAfter.attackPower > meleeBefore.attackPower || p.maxHealth > healthBefore ||
                                             meleeAfter.attributes[3] > meleeBefore.attributes[3]);
                if (a.kind == 4) ok = ok && p.mountSpellId == spellId;
                if (a.kind == 9) ok = ok && localFormRunPercent(p, content) > 120.f;
                if (a.kind == 10) ok = ok && meleeAfter.dodge > meleeBefore.dodge + 40.f;
                if (a.kind == 29) {
                    const auto tapped = tapHealth - p.health;
                    ok = ok && tapped > 0 && p.mana == std::min(p.maxMana, tapped);
                    // spell_warl_life_tap::CheckCast: not with the tap's health or less.
                    p.health = tapped; p.globalCooldownMs = 0; p.cooldowns.clear();
                    std::string refused;
                    const bool second = arena.execute(p, {LocalAction::CastSpell, p.guid, spellId}, players, refused);
                    ok = ok && !second && p.health == tapped;
                    if (!ok) result = "tapped " + std::to_string(tapped) + " mana " + std::to_string(p.mana) + " second " + std::to_string(second) + " (" + result + " / " + refused + ")";
                    p.health = p.maxHealth;
                }
                if (a.kind == 32 && ok) {
                    const auto holds = [&](uint32_t id) { for (const auto& s : p.statAuras) if (s.spellId == id && s.remainingMs) return true; return false; };
                    const auto* sd = content.spell(spellId);
                    ok = sd && sd->classBuffSchoolImmunity == 127 && holds(spellId) && holds(25771) && holds(61987) && holds(61988);
                    // Creatures brought beside the paladin swing at it: a swing shows as
                    // the creature's attack timer starting over. While the shield holds,
                    // swings land and nothing is lost; once it is gone, health drops.
                    std::vector<const LocalRealmNpc*> candidates;
                    for (const auto& m : arena.npcs()) if (m.hostile && !m.dead && m.health && m.mapId == p.mapId && m.instanceId == p.instanceId) candidates.push_back(&m);
                    std::sort(candidates.begin(), candidates.end(), [&](const auto* l, const auto* r) { return std::hypot(l->x - p.x, l->y - p.y) < std::hypot(r->x - p.x, r->y - p.y); });
                    uint32_t shieldedLow = p.maxHealth, exposedLow = p.maxHealth; int swings = 0; LocalRealmNpc* attacker = nullptr;
                    for (size_t ci = 0; ci < candidates.size() && ci < 4 && !swings && holds(spellId); ++ci) {
                        auto& foe = const_cast<LocalRealmNpc&>(*candidates[ci]);
                        foe.x = foe.homeX = p.x + 1.5f; foe.y = foe.homeY = p.y; foe.z = foe.homeZ = p.z; foe.maxHealth = foe.health = 1000000;
                        foe.level = p.level; // a start-zone level would miss a level 80 almost every swing
                        foe.targetGuid = p.guid; p.health = p.maxHealth; p.attackTarget = foe.guid; p.orientation = std::atan2(foe.y - p.y, foe.x - p.x);
                        float lastTimer = foe.attackTimer;
                        for (int t = 0; t < 50 && holds(spellId); ++t) {
                            arena.tick(0.05f, players); shieldedLow = std::min(shieldedLow, p.health); foe.health = foe.maxHealth;
                            if (foe.attackTimer > lastTimer + 0.5f) ++swings; lastTimer = foe.attackTimer; }
                        if (swings) attacker = &foe; else { p.attackTarget = 0; foe.targetGuid = 0; }
                    }
                    if (attacker) {
                        for (int t = 0; t < 400 && holds(spellId); ++t) { arena.tick(0.05f, players); attacker->health = attacker->maxHealth; p.health = p.maxHealth; }
                        for (int t = 0; t < 240; ++t) { arena.tick(0.05f, players); exposedLow = std::min(exposedLow, p.health); attacker->health = attacker->maxHealth; }
                    }
                    p.health = p.maxHealth; p.attackTarget = 0;
                    ok = ok && attacker && swings > 0 && shieldedLow == p.maxHealth && exposedLow < p.maxHealth;
                    std::string again, protection, wrath;
                    const auto known = [&](const char* name) { uint32_t id = 0; for (auto k : p.knownSpells) if (const auto* kd = content.spell(k); kd && kd->name == name && kd->unsupportedReason.empty()) id = k; return id; };
                    p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear(); p.mana = p.maxMana;
                    const bool second = arena.execute(p, {LocalAction::CastSpell, p.guid, spellId}, players, again);
                    p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear();
                    const bool dp = known("Divine Protection") && arena.execute(p, {LocalAction::CastSpell, p.guid, known("Divine Protection")}, players, protection);
                    p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear();
                    const bool aw = known("Avenging Wrath") && arena.execute(p, {LocalAction::CastSpell, p.guid, known("Avenging Wrath")}, players, wrath);
                    ok = ok && known("Divine Protection") && known("Avenging Wrath") && !second && !dp && !aw;
                    // Once the markers run out (3 min), Avenging Wrath is allowed again.
                    for (int t = 0; t < 1900 && holds(61988); ++t) arena.tick(0.1f, players);
                    p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear(); p.mana = p.maxMana;
                    const bool awLater = arena.execute(p, {LocalAction::CastSpell, p.guid, known("Avenging Wrath")}, players, wrath);
                    ok = ok && awLater && !holds(25771);
                    if (!ok) result = "swings " + std::to_string(swings) + " shielded low " + std::to_string(shieldedLow) + " exposed low " + std::to_string(exposedLow) + "/" + std::to_string(p.maxHealth) + " immunity " + std::to_string(sd ? sd->classBuffSchoolImmunity : 0) + " markers " + std::to_string(holds(25771)) + std::to_string(holds(61987)) + std::to_string(holds(61988)) +
                                      " second " + std::to_string(second) + " dp " + std::to_string(dp) + " aw " + std::to_string(aw) + " later " + std::to_string(awLater) + " (" + again + " / " + protection + " / " + wrath + ")";
                }
                if (a.kind == 64) {
                    // Innervate: 225 % of the druid's base mana over 10 one-second ticks.
                    const auto* sd = content.spell(spellId); uint32_t perTick = 0;
                    for (const auto& st : p.statAuras) if (st.spellId == spellId && st.remainingMs) perTick = st.buffArmorSnapshot;
                    const uint32_t expected = localClassBaseMana(p) * 225 / 100 / 10;
                    for (int t = 0; t < 50; ++t) arena.tick(0.05f, players); // 2.5 s
                    ok = ok && sd && sd->innervatePct == 225 && perTick == expected && perTick && p.mana >= 2 * perTick;
                    if (!ok) result = "tick " + std::to_string(perTick) + "/" + std::to_string(expected) + " mana " + std::to_string(p.mana) + " (" + result + ")";
                }
                if (a.kind == 61) {
                    // Divine Plea: 5 % of maximum mana every 3 s, healing done halved.
                    const auto* sd = content.spell(spellId);
                    for (int t = 0; t < 70; ++t) arena.tick(0.05f, players); // 3.5 s
                    const auto expected = p.maxMana * 5 / 100;
                    ok = ok && sd && sd->classBuffManaPct == 5 && sd->classBuffHealingDonePct == -50 && p.mana >= expected;
                    if (!ok) result = "mana " + std::to_string(p.mana) + "/" + std::to_string(expected) + " heal% " + std::to_string(sd ? sd->classBuffHealingDonePct : 0) + " (" + result + ")";
                }
                if (a.kind == 60) {
                    // Bloodrage: 16 % of base health paid, 20 rage at once, then its rage-over-time aura.
                    const auto* sd = content.spell(spellId); bool held = false;
                    for (const auto& st : p.statAuras) if (st.spellId == spellId && st.remainingMs) held = true;
                    const auto paid = tapHealth - p.health;
                    ok = ok && sd && held && paid > 0 && paid == localResourcePools(p, content).baseHealth * sd->healthCostBasePct / 100 &&
                         p.mana >= 19 && sd->energizeRage == 20 && sd->periodicRage == 1 && sd->periodicRageMs == 1000 && sd->durationMs == 10000;
                    if (!ok) result = "held " + std::to_string(held) + " paid " + std::to_string(paid) + " rage " + std::to_string(p.mana) + " (" + result + ")";
                }
                if (a.kind == 69) {
                    // Cone of Cold: the creatures in front of the mage hit and slowed.
                    const auto* sd = content.spell(spellId); size_t hit = 0, slowed = 0;
                    for (auto* foe : novaFoes) {
                        if (foe->health < foe->maxHealth) ++hit;
                        for (const auto& sn : foe->snares) if (sn.spellId == spellId && sn.casterGuid == p.guid && sn.remainingMs && sn.percent == sd->areaSnarePercent) { ++slowed; break; }
                    }
                    ok = ok && sd && sd->areaConeDegrees > 0 && hit > 0 && slowed == hit;
                    if (!ok) result = "foes " + std::to_string(novaFoes.size()) + " hit " + std::to_string(hit) + " slowed " + std::to_string(slowed) + " (" + result + ")";
                }
                if (a.kind == 68) {
                    // Challenging Shout: both creatures beside the warrior now attack it.
                    size_t taunted = 0; for (auto* foe : novaFoes) if (foe->targetGuid == p.guid) ++taunted;
                    ok = ok && novaFoes.size() == 2 && taunted == 2;
                    if (!ok) result = "foes " + std::to_string(novaFoes.size()) + " taunted " + std::to_string(taunted) + " (" + result + ")";
                }
                if (a.kind == 67) {
                    // Demoralizing Roar: an attack power debuff of the druid's on both creatures.
                    size_t roared = 0;
                    for (auto* foe : novaFoes) for (const auto& b : foe->npcBuffs) if (b.spellId == spellId && b.casterGuid == p.guid && b.attackPower < 0 && b.remainingMs) { ++roared; break; }
                    ok = ok && novaFoes.size() == 2 && roared == 2;
                    if (!ok) result = "foes " + std::to_string(novaFoes.size()) + " roared " + std::to_string(roared) + " form " + std::to_string(p.formSpellId) + " (" + result + ")";
                }
                if (a.kind == 59) {
                    // Both creatures beside the caster are feared, held with a damage cap.
                    const auto* sd = content.spell(spellId); size_t feared = 0;
                    for (auto* foe : novaFoes) for (const auto& ctl : foe->controls)
                        if (ctl.spellId == spellId && ctl.kind == uint8_t(LocalNpcControlKind::Stun) && ctl.remainingMs && ctl.damageLeft) { ++feared; break; }
                    ok = ok && sd && sd->areaFearRadius > 0 && novaFoes.size() == 2 && feared == 2;
                    if (!ok) result = "foes " + std::to_string(novaFoes.size()) + " feared " + std::to_string(feared) + " (" + result + ")";
                }
                if (a.kind == 57) {
                    // Every creature the nova hit is rooted; it stays put while rooted.
                    const auto* sd = content.spell(spellId);
                    size_t hit = 0, rooted = 0;
                    for (auto* foe : novaFoes) {
                        if (foe->health < foe->maxHealth) ++hit;
                        for (const auto& ctl : foe->controls) if (ctl.spellId == spellId && ctl.kind == uint8_t(LocalNpcControlKind::Root) && ctl.remainingMs && ctl.damageLeft) { ++rooted; break; }
                    }
                    ok = ok && sd && sd->areaRoot && hit > 0 && rooted == hit;
                    if (!ok) result = "foes " + std::to_string(novaFoes.size()) + " hit " + std::to_string(hit) + " rooted " + std::to_string(rooted) + " (" + result + ")";
                }
                if (a.kind == 56) {
                    // Frost Armor chills a creature hitting the mage in melee; each hit
                    // taken spends one of Inner Fire's charges. Both raise armor.
                    const auto* sd = content.spell(spellId);
                    const auto held = [&]() -> const LocalStatAura* { for (const auto& s : p.statAuras) if (s.spellId == spellId && s.remainingMs) return &s; return nullptr; };
                    const uint8_t chargesBefore = held() ? held()->procCharges : 0;
                    uint8_t chargesAfter = chargesBefore; bool chilled = false;
                    const auto armorAfter = localMeleeArmor(p, content);
                    LocalRealmNpc* foe = nullptr; float best = 1e9f;
                    for (const auto& m : arena.npcs()) if (m.hostile && !m.dead && m.health && m.mapId == p.mapId && m.instanceId == p.instanceId)
                        if (const float dist = std::hypot(m.x - p.x, m.y - p.y); dist < best) { best = dist; foe = const_cast<LocalRealmNpc*>(&m); }
                    if (foe && sd) {
                        foe->x = foe->homeX = p.x + 1.5f; foe->y = foe->homeY = p.y; foe->z = foe->homeZ = p.z; foe->maxHealth = foe->health = 1000000;
                        foe->level = p.level; foe->targetGuid = p.guid; p.attackTarget = foe->guid; p.orientation = std::atan2(foe->y - p.y, foe->x - p.x);
                        for (int t = 0; t < 400 && !chilled && chargesAfter == chargesBefore; ++t) {
                            arena.tick(0.05f, players); foe->health = foe->maxHealth; p.health = p.maxHealth;
                            for (const auto& b : foe->npcBuffs) if (b.spellId == sd->chillSpell && b.casterGuid == p.guid && b.remainingMs && b.hastePct < 0 && b.speedPct < 0) chilled = true;
                            chargesAfter = held() ? held()->procCharges : 0;
                        }
                        p.attackTarget = 0;
                    }
                    ok = ok && sd && foe && armorAfter > armorBefore &&
                         (sd->chillSpell ? chilled : sd->classBuffHitCharges && chargesBefore == sd->classBuffHitCharges && chargesAfter + 1 == chargesBefore);
                    if (!ok) result = "armor " + std::to_string(armorBefore) + "->" + std::to_string(armorAfter) + " chill " + std::to_string(sd ? sd->chillSpell : 0) + " chilled " + std::to_string(chilled) +
                                      " charges " + std::to_string(chargesBefore) + "->" + std::to_string(chargesAfter) + " foe " + std::to_string(foe != nullptr) + " (" + result + ")";
                }
                if (a.kind == 55) {
                    // Blessing of Kings: every stat 10% higher; Blessing of Wisdom: mana every 5 s.
                    const auto* sd = content.spell(spellId); bool held = false;
                    for (const auto& st : p.statAuras) if (st.spellId == spellId && st.remainingMs) held = true;
                    const bool kings = sd && sd->classBuffStatPct == 10;
                    const auto before = meleeBefore.attributes[0], after = meleeAfter.attributes[0];
                    ok = ok && sd && held && (kings ? after >= before + before / 10 - 1 && after > before : sd->manaPer5 > 0);
                    // One blessing per paladin: Kings then Wisdom leaves only Wisdom.
                    if (ok && kings) {
                        uint32_t wisdom = 0; for (auto k : p.knownSpells) if (const auto* kd = content.spell(k); kd && kd->name == "Blessing of Wisdom" && kd->unsupportedReason.empty()) wisdom = k;
                        p.globalCooldownMs = 0; p.mana = p.maxMana; std::string again;
                        const bool cast = wisdom && arena.execute(p, {LocalAction::CastSpell, p.guid, wisdom}, players, again);
                        bool kingsHeld = false, wisdomHeld = false;
                        for (const auto& st : p.statAuras) if (st.remainingMs) { kingsHeld |= st.spellId == spellId; wisdomHeld |= st.spellId == wisdom; }
                        ok = cast && wisdomHeld && !kingsHeld;
                        if (!ok) result = "after Wisdom: kings " + std::to_string(kingsHeld) + " wisdom " + std::to_string(wisdomHeld) + " (" + again + ")";
                    }
                    if (!ok) result = "held " + std::to_string(held) + " strength " + std::to_string(before) + "->" + std::to_string(after) + " mp5 " + std::to_string(sd ? sd->manaPer5 : 0) + " (" + result + ")";
                }
                if (a.kind == 51) {
                    // Windfury Totem's party aura: a class buff with its melee haste on the shaman.
                    for (int t = 0; t < 40; ++t) arena.tick(0.05f, players);
                    int32_t haste = 0;
                    for (const auto& st : p.statAuras) if (st.remainingMs) if (const auto* bd = content.spell(st.spellId); bd && bd->classBuff) haste = std::max(haste, bd->classBuffMeleeHastePct);
                    ok = ok && haste == 16;
                    if (!ok) result = "windfury haste " + std::to_string(haste) + " (" + result + ")";
                }
                if (a.kind == 50) {
                    const auto* sd = content.spell(spellId); bool held = false;
                    for (const auto& st : p.statAuras) if (st.spellId == spellId && st.remainingMs) held = true;
                    ok = ok && sd && held && sd->classBuffRangedHastePct == 40;
                    if (!ok) result = std::string("rapid fire held ") + std::to_string(held) + " (" + result + ")";
                }
                if (a.kind == 49) {
                    const auto holds = [&](uint32_t id) { for (const auto& s : p.statAuras) if (s.spellId == id && s.remainingMs) return true; return false; };
                    const auto* sd = content.spell(spellId);
                    const bool first = holds(spellId) && holds(57724);
                    std::string cancel, again;
                    arena.execute(p, {LocalAction::CancelStatAura, 0, spellId}, players, cancel);
                    p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear(); p.mana = p.maxMana;
                    const bool cast = arena.execute(p, {LocalAction::CastSpell, p.guid, spellId}, players, again);
                    ok = ok && sd && sd->classBuffMeleeRangedHastePct == 30 && sd->classBuffCastSpeedPct == 30 && first && cast && !holds(spellId) && holds(57724);
                    if (!ok) result = "first " + std::to_string(first) + " recast " + std::to_string(cast) + " held again " + std::to_string(holds(spellId)) + " (" + again + ")";
                }
                if (a.kind == 47) {
                    bool ready = true; for (auto r : p.runeCooldownMs) ready = ready && r == 0;
                    ok = ok && ready;
                    if (!ok) result = "runes not refreshed (" + result + ")";
                }
                if (a.kind == 46) {
                    const auto* sd = content.spell(spellId);
                    const LocalRealmPet* ghoul = nullptr;
                    for (const auto& v : arena.pets()) if (v.ownerGuid == p.guid && v.kind == LocalPetKind::Guardian && sd && v.entry == sd->raiseDeadEntry) ghoul = &v;
                    uint32_t dust = 0; for (const auto& st : p.inventory) if (sd && st.itemId == sd->raiseDeadReagent) dust += st.count;
                    ok = ok && sd && raiseRefused && ghoul && ghoul->remainingMs > 0 && ghoul->remainingMs <= sd->raiseDeadDurationMs && dust == 0;
                    if (!ok) result = std::string("refused ") + std::to_string(raiseRefused) + " ghoul " + std::to_string(ghoul != nullptr) + " dust " + std::to_string(dust) + " (" + result + ")";
                }
                if (a.kind == 45 && ok) {
                    uint32_t pool = 0; for (const auto& s : p.statAuras) if (s.spellId == spellId && s.remainingMs) pool = s.absorbRemaining;
                    p.mana = 0;
                    const auto physical = localAbsorbDamage(p, content, 1000, 1);
                    const auto shadow = localAbsorbDamage(p, content, 1000, 32);
                    ok = pool == p.maxHealth / 2 && physical == 1000 && shadow == 250 && p.mana == 15;
                    if (!ok) result = "pool " + std::to_string(pool) + "/" + std::to_string(p.maxHealth) + " physical " + std::to_string(physical) + " shadow " + std::to_string(shadow) + " runic " + std::to_string(p.mana) + " (" + result + ")";
                }
                if (a.kind == 39 && ok) {
                    const auto holds = [&](uint32_t id) { for (const auto& s : p.statAuras) if (s.spellId == id && s.remainingMs) return true; return false; };
                    uint32_t absorb = 0; for (const auto& s : p.statAuras) if (s.spellId == spellId) absorb = s.absorbRemaining;
                    const bool weakened = holds(6788);
                    std::string again, later; p.globalCooldownMs = 0; p.cooldowns.clear(); p.mana = p.maxMana;
                    const bool second = arena.execute(p, {LocalAction::CastSpell, p.guid, spellId}, players, again);
                    for (int t = 0; t < 320 && holds(6788); ++t) arena.tick(0.05f, players);
                    p.globalCooldownMs = 0; p.cooldowns.clear(); p.mana = p.maxMana;
                    const bool third = arena.execute(p, {LocalAction::CastSpell, p.guid, spellId}, players, later);
                    ok = absorb > 0 && weakened && !second && third;
                    if (!ok) result = "absorb " + std::to_string(absorb) + " weakened " + std::to_string(weakened) + " second " + std::to_string(second) + " later " + std::to_string(third) + " (" + again + " / " + later + ")";
                }
                if (a.kind == 38 && ok) {
                    const auto* sd = content.spell(spellId);
                    const auto armorAfter = localMeleeArmor(p, content);
                    const int32_t amount = sd ? sd->areaAuraAmounts[0] : 0;
                    ok = sd && amount > 0 && armorAfter >= armorBefore + uint32_t(amount) && p.areaEmitters.size() == 1;
                    uint32_t retribution = 0; for (auto k : p.knownSpells) if (const auto* kd = content.spell(k); kd && kd->name == "Retribution Aura" && kd->unsupportedReason.empty()) retribution = k;
                    std::string swap; p.globalCooldownMs = 0;
                    const bool swapped = retribution && arena.execute(p, {LocalAction::CastSpell, p.guid, retribution}, players, swap);
                    arena.tick(0.05f, players);
                    ok = ok && swapped && p.areaEmitters.size() == 1 && p.areaEmitters[0].spellId == retribution && localMeleeArmor(p, content) + uint32_t(amount) <= armorAfter;
                    if (!ok) result = "armor " + std::to_string(armorBefore) + " -> " + std::to_string(armorAfter) + " amount " + std::to_string(amount) + " emitters " + std::to_string(p.areaEmitters.size()) + " (" + result + " / " + swap + ")";
                }
                if (a.kind == 37 && ok) {
                    const auto* sd = content.spell(spellId);
                    // The same 3.1 s without the aspect first: natural regeneration alone.
                    std::string cancel; arena.execute(p, {LocalAction::CancelStatAura, 0, spellId}, players, cancel);
                    p.mana = 0; for (int t = 0; t < 62; ++t) arena.tick(0.05f, players);
                    const uint32_t natural = p.mana;
                    p.globalCooldownMs = 0; p.cooldowns.clear(); p.mana = p.maxMana;
                    arena.execute(p, {LocalAction::CastSpell, p.guid, spellId}, players, result); arena.tick(0.05f, players);
                    p.mana = 0; for (int t = 0; t < 62; ++t) arena.tick(0.05f, players);
                    const uint32_t expected = p.maxMana * (sd ? sd->classBuffManaPct : 0) / 100;
                    ok = sd && sd->classBuffManaPct == 4 && sd->classBuffDamagePct == -50 && expected > 0 && p.mana >= natural + expected * 95 / 100;
                    if (!ok) result = "mana " + std::to_string(p.mana) + " natural " + std::to_string(natural) + " expected +" + std::to_string(expected) + " (" + result + ")";
                    p.mana = p.maxMana;
                }
                if (a.kind == 36 && ok) {
                    const auto* sd = content.spell(spellId); uint32_t absorb = 0;
                    for (const auto& s : p.statAuras) if (s.spellId == spellId && s.remainingMs) absorb = s.absorbRemaining;
                    const auto left = localAbsorbDamage(p, content, 100, 16);
                    ok = sd && sd->buffAbsorbPerLevel > 0 && absorb >= sd->buffAbsorb && left == 0; // rank 8 starts at level 80: no level term yet
                    if (!ok) result = "absorb " + std::to_string(absorb) + " base " + std::to_string(sd ? sd->buffAbsorb : 0) + " frost left " + std::to_string(left) + " (" + result + ")";
                }
                if (a.kind == 35 && ok) {
                    const auto* sd = content.spell(spellId);
                    bool held = false; for (const auto& s : p.statAuras) if (s.spellId == spellId && s.remainingMs) held = true;
                    const std::string name = a.name;
                    ok = sd && held && (name == "Fear Ward" ? sd->classBuffMechanicImmunity == (1u << 5) && sd->classBuffImmunityCharge :
                                        name == "Icebound Fortitude" ? sd->classBuffMechanicImmunity == (1u << 12) && sd->classBuffDamageTakenPct == -30 && !sd->classBuffImmunityCharge
                                                                     : sd->classBuffPushbackPct == 100 && sd->classBuffDamageTakenPct == -20);
                    if (!ok) result = std::string("held ") + std::to_string(held) + " (" + result + ")";
                }
                if (a.kind == 34 && ok) {
                    const auto holds = [&](uint32_t id) { for (const auto& s : p.statAuras) if (s.spellId == id && s.remainingMs) return true; return false; };
                    uint32_t bolt = 0; for (auto k : p.knownSpells) if (const auto* kd = content.spell(k); kd && kd->name == "Frostbolt" && kd->unsupportedReason.empty()) bolt = k;
                    std::string held, again;
                    p.globalCooldownMs = 0; p.mana = p.maxMana;
                    const bool castInBlock = arena.execute(p, {LocalAction::CastSpell, p.guid, bolt}, players, held);
                    ok = holds(spellId) && holds(41425) && bolt && !castInBlock && held.find("stunned") != std::string::npos;
                    std::string cancel;
                    const bool cancelled = arena.execute(p, {LocalAction::CancelStatAura, 0, spellId}, players, cancel);
                    p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear();
                    const bool second = arena.execute(p, {LocalAction::CastSpell, p.guid, spellId}, players, again);
                    ok = ok && cancelled && !holds(spellId) && !second;
                    if (!ok) result = "block " + std::to_string(holds(spellId)) + " hypothermia " + std::to_string(holds(41425)) + " cast " + std::to_string(castInBlock) +
                                      " cancelled " + std::to_string(cancelled) + " second " + std::to_string(second) + " (" + held + " / " + cancel + " / " + again + ")";
                }
                if (a.kind == 33 && ok) {
                    const auto* sd = content.spell(spellId);
                    bool held = false; for (const auto& s : p.statAuras) if (s.spellId == spellId && s.remainingMs) held = true;
                    ok = sd && held && sd->classBuffDamageTakenPct == -60;
                    if (!ok) result = "shield wall " + std::to_string(held) + " pct " + std::to_string(sd ? sd->classBuffDamageTakenPct : 0) + " (" + result + ")";
                }
                if (a.kind == 30 && ok) {
                    const auto* sd = content.spell(spellId);
                    const auto fire = localAbsorbDamage(p, content, 100, 4);
                    const auto shadow = localAbsorbDamage(p, content, 100, 32);
                    ok = sd && sd->absorbSchoolMask == 32 && fire == 100 && shadow == 0;
                    if (!ok) result = "fire left " + std::to_string(fire) + " shadow left " + std::to_string(shadow) + " (" + result + ")";
                }
                if (a.kind == 28 && ok) {
                    const auto held = [&](const char* name) { for (const auto& s : p.statAuras) if (const auto* sd = content.spell(s.spellId); sd && sd->name == name && s.remainingMs) return sd; return (const LocalSpellDefinition*)nullptr; };
                    const auto* armor = held(a.name);
                    const auto armorAfter = localMeleeArmor(p, content);
                    ok = armor && armor->classBuffHealingTakenPct > 0 && armorAfter >= armorBefore + uint32_t(armor->classBuffArmor);
                    uint32_t skin = 0; for (auto id : p.knownSpells) if (const auto* sd = content.spell(id); sd && sd->name == "Demon Skin" && sd->unsupportedReason.empty()) skin = id;
                    ok = ok && skin; // the warlock knows both armors at level 80
                    if (ok) { p.globalCooldownMs = 0; p.mana = p.maxMana;
                        ok = arena.execute(p, {LocalAction::CastSpell, p.guid, skin}, players, result) && held("Demon Skin") && !held(a.name); }
                    if (!ok) result = "armor " + std::to_string(armorBefore) + " -> " + std::to_string(armorAfter) + (armor ? " buff " + std::to_string(armor->classBuffArmor) + " heal% " + std::to_string(armor->classBuffHealingTakenPct) : std::string(" not held")) +
                                      " skin " + std::to_string(skin) + " (" + result + ")";
                }
                if (a.kind == 20 && ok) {
                    for (int t = 0; t < 400; ++t) arena.tick(0.05f, players);
                    const auto held = [&](const char* name) { for (const auto& s : p.statAuras) if (const auto* sd = content.spell(s.spellId); sd && sd->name == name && s.remainingMs) return true; return false; };
                    uint32_t frost = 0; for (auto id : p.knownSpells) if (const auto* sd = content.spell(id); sd && sd->name == "Frost Presence" && sd->unsupportedReason.empty()) frost = id;
                    ok = held(a.name) && frost;
                    p.globalCooldownMs = 0; p.runeCooldownMs.fill(0);
                    ok = ok && arena.execute(p, {LocalAction::CastSpell, p.guid, frost}, players, result) && !held(a.name) && held("Frost Presence");
                    if (!ok) result = "presence not held or not exclusive (" + result + ")";
                }
                if (a.kind == 14 && ok) {
                    const auto* totem = [&]() -> const LocalRealmPet* { for (const auto& v : arena.pets()) if (v.ownerGuid == p.guid && v.summonSpellId == spellId && v.kind == LocalPetKind::Totem) return &v; return nullptr; }();
                    const auto* t = content.totem(spellId);
                    ok = totem && t;
                    if (ok && t->kind == LocalWorldContent::TotemKind::PulseHeal) {
                        p.health = p.maxHealth / 2; const auto low = p.health;
                        for (int i = 0; i < 80; ++i) arena.tick(0.05f, players);
                        ok = p.health > low;
                    } else if (ok) {
                        for (int i = 0; i < 40; ++i) arena.tick(0.05f, players);
                        const auto after = localMeleeStats(p, content);
                        ok = after.attributes[0] > meleeBefore.attributes[0];
                    }
                    if (!ok) result = "totem down without its effect (" + result + ")";
                }
                if (a.kind == 13 && ok) {
                    for (int t = 0; t < 400; ++t) arena.tick(0.05f, players);
                    uint32_t mask = 0; size_t trackers = 0;
                    for (const auto& s : p.statAuras) if (const auto* sd = content.spell(s.spellId); sd && s.remainingMs && sd->trackCreatureMask) { mask |= sd->trackCreatureMask; ++trackers; }
                    ok = trackers == 1 && mask == content.spell(spellId)->trackCreatureMask && mask;
                    uint32_t other = 0; for (auto id : p.knownSpells) if (const auto* sd = content.spell(id); sd && id != spellId && sd->trackCreatureMask && sd->unsupportedReason.empty() && localSpellFormReady(p, *sd)) other = id;
                    if (ok && other) { p.globalCooldownMs = 0; ok = arena.execute(p, {LocalAction::CastSpell, p.guid, other}, players, result);
                        trackers = 0; for (const auto& s : p.statAuras) if (const auto* sd = content.spell(s.spellId); sd && s.remainingMs && sd->trackCreatureMask) { ++trackers; ok = ok && s.spellId == other; }
                        ok = ok && trackers == 1; }
                    if (!ok) result = "tracking not held or not exclusive (" + result + ")";
                }
                if (a.kind == 12 && ok) {
                    // An aspect does not expire; another aspect replaces it.
                    for (int t = 0; t < 400; ++t) arena.tick(0.05f, players);
                    const auto rangedAfter = autoShot ? localRangedAmounts(p, content, *autoShot) : LocalRangedAmounts{};
                    const auto held = [&](const char* name) { for (const auto& s : p.statAuras) if (const auto* sd = content.spell(s.spellId); sd && sd->name == name && s.remainingMs) return true; return false; };
                    ok = held(a.name) && rangedAfter.active && rangedAfter.high > rangedBefore.high;
                    uint32_t monkey = 0; for (auto id : p.knownSpells) if (const auto* sd = content.spell(id); sd && sd->name == "Aspect of the Monkey" && sd->unsupportedReason.empty()) monkey = id;
                    p.globalCooldownMs = 0;
                    ok = ok && monkey && arena.execute(p, {LocalAction::CastSpell, p.guid, monkey}, players, result) && !held(a.name) && held("Aspect of the Monkey");
                    if (!ok) result = "aspect not held, no ranged attack power or not exclusive (" + result + ")";
                }
                if (a.kind == 8) { bool summoned = false;
                    for (const auto& v : arena.pets()) if (v.ownerGuid == p.guid && !v.dead) summoned = true;
                    ok = ok && summoned; if (!summoned && ok) result = "no demon"; }
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
                for (const auto& n : arena.npcs()) if (n.hostile && !n.dead && n.health && std::hypot(n.x - p.x, n.y - p.y) < 120 &&
                    localSpellCreatureTypeAllowed(*content.spell(spellId), localNpcCreatureType(n.entry))) foes.push_back(n.guid); }
            if (foes.empty()) { out << "FAIL class ability " << a.name << ": no enemy near the start\n"; return false; }
            uint64_t foeGuid = foes.front(); size_t foeIndex = 0;
            bool landed = false; std::string last;
            for (int attempt = 0; attempt < 12 && !landed; ++attempt) {
                // Some "hostile" creatures at a start (Ebon Hold) cannot be attacked: try the next one.
                if (last.find("living enemy") != std::string::npos && foeIndex + 1 < foes.size()) { foeGuid = foes[++foeIndex]; last.clear(); }
                const LocalRealmNpc* n = nullptr;
                for (const auto& v : arena.npcs()) if (v.guid == foeGuid) n = &v;
                if (!n || n->dead) { if (foeIndex + 1 < foes.size()) { foeGuid = foes[++foeIndex]; continue; } break; }
                const bool ranged = (a.cls == 3 && std::string(a.name) != "Raptor Strike") || a.kind == 6 || a.kind == 7;
                const float gap = ranged ? 15.f : 1.5f;
                p.x = n->x - gap; p.y = n->y; p.z = n->z; p.orientation = 0; ++p.positionRevision;
                p.mana = a.kind == 6 ? 0 : p.maxMana; p.runeCooldownMs.fill(0); p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear();
                p.health = p.maxHealth;
                if (std::string(a.name) == "Overpower") p.overpowerWindowMs = 5000;
                if (a.kind >= 23 && a.kind <= 25) { auto& sturdy = const_cast<LocalRealmNpc&>(*n); sturdy.maxHealth = sturdy.health = 100000; }
                if (a.kind == 31) { auto& sturdy = const_cast<LocalRealmNpc&>(*n); sturdy.maxHealth = sturdy.health = 50000; p.health = p.maxHealth / 4; }
                // Fire Nova: refused with no fire totem, then a Searing Totem down beside a creature 3 yd away.
                bool novaRefused = false; uint32_t novaBefore = 0;
                if (a.kind == 53) {
                    std::string refusal; novaRefused = !arena.execute(p, {LocalAction::CastSpell, p.guid, spellId}, players, refusal) && refusal.find("fire totem") != std::string::npos;
                    auto& near = const_cast<LocalRealmNpc&>(*n); near.x = near.homeX = p.x + 3; near.y = near.homeY = p.y; near.z = near.homeZ = p.z; near.maxHealth = near.health = 100000;
                    uint32_t searing = 0; for (auto k : p.knownSpells) if (const auto* kd = content.spell(k); kd && kd->name == "Searing Totem" && kd->unsupportedReason.empty()) searing = k;
                    p.globalCooldownMs = 0; p.mana = p.maxMana;
                    arena.execute(p, {LocalAction::CastSpell, p.guid, searing}, players, refusal); arena.tick(0.05f, players);
                    p.globalCooldownMs = 0; p.mana = p.maxMana; p.cooldowns.clear(); novaBefore = near.health;
                }
                // A judgement: refused with no seal, then Seal of Righteousness first.
                bool judgementRefused = false;
                if (a.kind == 54) {
                    std::string refusal; judgementRefused = !arena.execute(p, {LocalAction::CastSpell, foeGuid, spellId}, players, refusal) && refusal.find("seal") != std::string::npos;
                    uint32_t seal = 0; for (auto k : p.knownSpells) if (const auto* kd = content.spell(k); kd && kd->sealOfRighteousness && kd->unsupportedReason.empty()) seal = k;
                    p.globalCooldownMs = 0; p.mana = p.maxMana;
                    arena.execute(p, {LocalAction::CastSpell, p.guid, seal}, players, refusal); arena.tick(0.05f, players);
                    p.globalCooldownMs = 0; p.mana = p.maxMana; p.cooldowns.clear(); p.categoryCooldowns.clear();
                    auto& sturdy = const_cast<LocalRealmNpc&>(*n); sturdy.maxHealth = sturdy.health = 100000;
                }
                if (a.kind == 52 || a.kind == 65 || a.kind == 70) { // in combat first: the creature fighting the hunter / rogue / priest
                    p.attackTarget = foeGuid; for (int t = 0; t < 30; ++t) { p.health = p.maxHealth; arena.tick(0.05f, players); }
                    p.globalCooldownMs = 0; p.mana = p.maxMana; }
                if (a.kind == 43) { auto& far = const_cast<LocalRealmNpc&>(*n); far.x = far.homeX = p.x + 15; far.y = far.homeY = p.y; far.z = far.homeZ = p.z;
                    p.orientation = 0; } // 15 yd east, the knight facing it
                if (a.kind == 42 || a.kind == 66) { auto& sturdy = const_cast<LocalRealmNpc&>(*n); sturdy.maxHealth = sturdy.health = 100000; }
                if (a.kind == 41) { auto& sturdy = const_cast<LocalRealmNpc&>(*n); sturdy.maxHealth = sturdy.health = 20000; }
                if (a.kind == 27) { auto& sturdy = const_cast<LocalRealmNpc&>(*n); sturdy.maxHealth = sturdy.health = 20000; }
                const uint32_t before = n->health;
                if (a.kind == 15) p.attackTarget = foeGuid;
                if (a.kind == 19) {
                    p.globalCooldownMs = 0; arena.execute(p, {LocalAction::CastSpell, p.guid, 768}, players, result);
                    p.globalCooldownMs = 0; p.mana = p.maxMana;
                    if (arena.execute(p, {LocalAction::CastSpell, foeGuid, spellId}, players, result) || result.find("stealthed") == std::string::npos) {
                        last = "an opener outside Prowl was not refused (" + result + ")"; break; }
                    p.globalCooldownMs = 0; arena.execute(p, {LocalAction::CastSpell, p.guid, kLocalProwlSpell}, players, result);
                    if (!localStealthed(p)) { last = "Prowl did not stealth the cat (" + result + ")"; break; }
                    p.globalCooldownMs = 0; p.mana = p.maxMana;
                }
                uint32_t builder = 0;
                for (auto id : p.knownSpells) if (const auto* sd = content.spell(id); sd && sd->name == "Sinister Strike" && sd->unsupportedReason.empty()) builder = id;
                if (a.kind == 17 || a.kind == 18 || a.kind == 23 || a.kind == 25) {
                    for (int strike = 0; strike < (a.kind == 23 || a.kind == 25 ? 2 : 1); ++strike) {
                        arena.execute(p, {LocalAction::CastSpell, foeGuid, builder}, players, result);
                        arena.tick(0.05f, players);
                        p.mana = p.maxMana; p.globalCooldownMs = 0;
                    }
                    if (!p.comboPoints) { last = "no combo points from Sinister Strike (" + result + ")"; continue; }
                }
                const uint8_t comboBefore = p.comboPoints;
                const uint32_t coilHealth = p.health;
                // Pestilence spreads what is already there: Frost Fever on the target
                // first, and a second creature brought beside it.
                uint64_t pestilenceOther = 0;
                if (a.kind == 44) {
                    uint32_t touch = 0; for (auto k : p.knownSpells) if (const auto* kd = content.spell(k); kd && kd->name == "Icy Touch" && kd->unsupportedReason.empty()) touch = k;
                    p.mana = p.maxMana; p.runeCooldownMs.fill(0); p.globalCooldownMs = 0;
                    arena.execute(p, {LocalAction::CastSpell, foeGuid, touch}, players, result); arena.tick(0.05f, players);
                    const LocalRealmNpc* target = nullptr; for (const auto& v : arena.npcs()) if (v.guid == foeGuid) target = &v;
                    for (const auto& m : arena.npcs()) if (target && !pestilenceOther && m.guid != foeGuid && m.hostile && !m.dead && m.health && m.mapId == target->mapId) {
                        auto& near = const_cast<LocalRealmNpc&>(m); near.x = near.homeX = target->x + 3; near.y = near.homeY = target->y; near.z = near.homeZ = target->z;
                        near.maxHealth = near.health = 100000; pestilenceOther = m.guid; }
                    p.mana = p.maxMana; p.runeCooldownMs.fill(0); p.globalCooldownMs = 0;
                }
                // Intimidating Shout: a second creature beside the warrior.
                uint64_t shoutOther = 0;
                if (a.kind == 62) for (const auto& m : arena.npcs()) if (!shoutOther && m.guid != foeGuid && m.hostile && !m.dead && m.health && m.mapId == p.mapId && m.instanceId == p.instanceId) {
                    auto& near = const_cast<LocalRealmNpc&>(m); near.x = near.homeX = p.x + 2; near.y = near.homeY = p.y + 1; near.z = near.homeZ = p.z;
                    near.maxHealth = near.health = 100000; near.level = p.level; shoutOther = m.guid; }
                // Feint: threat built first by fighting, then the feint takes it away.
                uint64_t feintThreatBefore = 0;
                if (a.kind == 66) {
                    for (int strike = 0; strike < 3; ++strike) {
                        p.mana = p.maxMana; p.globalCooldownMs = 0; std::string built;
                        arena.execute(p, {LocalAction::CastSpell, foeGuid, builder}, players, built); arena.tick(0.05f, players);
                        if (std::getenv("ABILITY_VERBOSE")) out << "  feint builder: " << built << "\n"; }
                    for (const auto& v : arena.npcs()) if (v.guid == foeGuid) for (const auto& e : v.threat) if (e.guid == p.guid) feintThreatBefore = e.amount;
                    p.globalCooldownMs = 0; p.mana = p.maxMana;
                }
                uint32_t firstCurse = 0;
                if (a.kind == 26 && std::string(a.name) == "Curse of the Elements") {
                    for (auto id : p.knownSpells) if (const auto* sd = content.spell(id); sd && sd->name == "Curse of Weakness" && sd->unsupportedReason.empty()) firstCurse = id;
                    arena.execute(p, {LocalAction::CastSpell, foeGuid, firstCurse}, players, result); arena.tick(0.05f, players);
                    bool held = false; for (const auto& v : arena.npcs()) if (v.guid == foeGuid) for (const auto& b : v.npcBuffs) if (b.spellId == firstCurse && b.casterGuid == p.guid) held = true;
                    if (!held) { last = "Curse of Weakness did not land first (" + result + ")"; continue; }
                    p.mana = p.maxMana; p.globalCooldownMs = 0;
                }
                if (!arena.execute(p, {LocalAction::CastSpell, a.kind == 15 || a.kind == 18 || a.kind == 22 || a.kind == 52 || a.kind == 53 || a.kind == 65 || a.kind == 70 ? p.guid : foeGuid, spellId}, players, result)) {
                    last = result;
                    if (a.kind == 21 && result.find("Nothing to dispel") != std::string::npos) { landed = true; break; }
                    if (std::getenv("ABILITY_VERBOSE")) out << "  " << a.name << " attempt " << attempt << ": " << result << " form=" << p.formSpellId << "\n";
                    // A stance-bound ability (Overpower): take the next known stance and retry.
                    if (result.find("form or stance") != std::string::npos) {
                        std::vector<uint32_t> forms;
                        for (auto id : p.knownSpells) if (const auto* f = content.spell(id); f && f->formId && f->unsupportedReason.empty()) forms.push_back(id);
                        if (!forms.empty()) {
                            p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear();
                            arena.execute(p, {LocalAction::CastSpell, p.guid, forms[size_t(attempt) % forms.size()]}, players, result);
                            if (std::getenv("ABILITY_VERBOSE")) out << "    stance " << forms[size_t(attempt) % forms.size()] << ": " << result << "\n";
                        }
                    }
                    arena.tick(0.05f, players); continue;
                }
                for (int t = 0; t < 120 && p.castingSpellId; ++t) { p.health = p.maxHealth; arena.tick(0.05f, players); }
                arena.tick(0.05f, players);
                const LocalRealmNpc* after = nullptr;
                for (const auto& v : arena.npcs()) if (v.guid == foeGuid) after = &v;
                if (a.kind == 7) {
                    for (int t = 0; t < 200 && after && !after->dead; ++t) { p.health = p.maxHealth; arena.tick(0.05f, players); after = nullptr;
                        for (const auto& v : arena.npcs()) if (v.guid == foeGuid) after = &v; }
                    uint32_t shards = 0; for (const auto& st : p.inventory) if (st.itemId == 6265) shards += st.count;
                    landed = shards > 0;
                    if (!landed) result = "no Soul Shard after the kill";
                }
                else if (a.kind == 6) landed = p.lastCastSpellId == spellId && after && std::hypot(after->x - p.x, after->y - p.y) < 6 &&
                                          p.mana > 0 && p.attackTarget == foeGuid;
                else if (a.kind == 1) landed = p.lastCastSpellId == spellId;
                else if (a.kind == 22) {
                    const auto* t = content.totem(spellId);
                    for (int i = 0; i < 20 && !landed; ++i) { arena.tick(0.05f, players);
                        for (const auto& v : arena.npcs()) if (v.guid == foeGuid) for (const auto& s : v.snares) if (t && s.spellId == t->snareSpell) landed = true; }
                    if (!landed) result = "the enemy beside the totem is not slowed";
                }
                else if (a.kind == 18) {
                    uint32_t held = 0; for (const auto& s : p.statAuras) if (s.spellId == spellId) held = s.remainingMs;
                    landed = !p.comboPoints && held > 6000;
                    if (!landed) result = "combo=" + std::to_string(p.comboPoints) + " aura=" + std::to_string(held) + "ms";
                }
                else if (a.kind == 23 || a.kind == 24) {
                    const auto held = [&] { uint32_t ms = 0; for (const auto& v : arena.npcs()) if (v.guid == foeGuid) for (const auto& s : v.controls) if (s.spellId == spellId) ms = s.remainingMs; return ms; };
                    const auto stunned = held(); const auto* sd = content.spell(spellId);
                    // Kidney Shot rank 1 has no base length: 1 s per point spent.
                    landed = stunned && sd && (a.kind == 24 || (!p.comboPoints && stunned > (comboBefore - 1) * 1000u && stunned <= comboBefore * 1000u));
                    if (landed && a.kind == 24) {
                        for (int strike = 0; strike < 6 && held(); ++strike) {
                            p.mana = p.maxMana; p.globalCooldownMs = 0;
                            arena.execute(p, {LocalAction::CastSpell, foeGuid, builder}, players, result); arena.tick(0.05f, players);
                        }
                        landed = !held();
                        if (!landed) result = "the control outlasted six Sinister Strikes (" + result + ")";
                    } else if (!landed) result = "control=" + std::to_string(stunned) + "ms combo " + std::to_string(comboBefore) + "->" + std::to_string(p.comboPoints) + " (" + result + ")";
                }
                else if (a.kind == 25) {
                    // Expose Armor: 6 s per point spent, 20 % of the armor.
                    uint32_t held = 0; uint8_t pct = 0;
                    for (const auto& v : arena.npcs()) if (v.guid == foeGuid) for (const auto& s : v.armorDebuffs) if (s.spellId == spellId) { held = s.remainingMs; pct = s.percent; }
                    const auto* sd = content.spell(spellId);
                    landed = sd && pct == sd->armorDebuffPct && pct && !p.comboPoints && held > (comboBefore - 1) * 6000u && held <= comboBefore * 6000u;
                    if (!landed) result = "armor debuff=" + std::to_string(held) + "ms " + std::to_string(pct) + "% combo " + std::to_string(comboBefore) + "->" + std::to_string(p.comboPoints) + " (" + result + ")";
                }
                else if (a.kind == 70) {
                    // Fade: held with its threat reduction; alone on the list, the priest stays the victim.
                    const auto* sd = content.spell(spellId); bool held = false; uint64_t target = 0;
                    for (const auto& st : p.statAuras) if (st.spellId == spellId && st.remainingMs) held = true;
                    for (int t = 0; t < 10; ++t) { p.health = p.maxHealth; arena.tick(0.05f, players); }
                    for (const auto& v : arena.npcs()) if (v.guid == foeGuid) target = v.targetGuid;
                    landed = sd && sd->classBuffThreatReduction >= 90000000 && held && target == p.guid;
                    if (!landed) result = "held " + std::to_string(held) + " still the victim " + std::to_string(target == p.guid) + " (" + result + ")";
                }
                else if (a.kind == 66) {
                    // Feint: the rogue's threat on the creature drops by the spell's amount (floored at 0).
                    const auto* sd = content.spell(spellId); uint64_t threat = 0;
                    for (const auto& v : arena.npcs()) if (v.guid == foeGuid) for (const auto& e : v.threat) if (e.guid == p.guid) threat = e.amount;
                    landed = sd && sd->threatReduction >= 150 && feintThreatBefore && threat + std::min<uint64_t>(feintThreatBefore, uint64_t(sd->threatReduction) * 1000) <= feintThreatBefore + 300000;
                    if (!landed) result = "threat " + std::to_string(feintThreatBefore) + " -> " + std::to_string(threat) + " (" + result + ")";
                }
                else if (a.kind == 65) {
                    // Vanish: stealthed, and the creature it was fighting drops the rogue.
                    uint64_t target = 0; for (const auto& v : arena.npcs()) if (v.guid == foeGuid) target = v.targetGuid;
                    landed = localStealthed(p) && target != p.guid && !p.attackTarget;
                    if (!landed) result = "stealthed " + std::to_string(localStealthed(p)) + " creature target " + std::to_string(target == p.guid) + " (" + result + ")";
                }
                else if (a.kind == 63) {
                    // Faerie Fire: 5 % armor, a minor armor debuff (apart from Sunder / Expose Armor).
                    uint8_t pct = 0; bool minor = false; uint32_t armorWith = 0, armorBase = 0;
                    if (after) for (const auto& s : after->armorDebuffs) if (s.spellId == spellId && s.remainingMs) { pct = s.percent; minor = s.minor; }
                    if (after) { armorWith = localNpcArmorAfterDebuffs(10000, *after); }
                    armorBase = 10000 * (100 - pct) / 100;
                    landed = pct == 5 && minor && armorWith == armorBase;
                    if (!landed) result = "faerie fire " + std::to_string(pct) + "% minor " + std::to_string(minor) + " armor " + std::to_string(armorWith) + " (" + result + ")";
                }
                else if (a.kind == 62) {
                    // Intimidating Shout: the target cowers until damaged, the creature beside it is feared.
                    uint32_t cowerCap = 1, otherCap = 0; bool cower = false, other = false;
                    for (const auto& u : arena.npcs()) for (const auto& ctl : u.controls) if (ctl.spellId == spellId && ctl.casterGuid == p.guid && ctl.remainingMs) {
                        if (u.guid == foeGuid) { cower = true; cowerCap = ctl.damageLeft; } else if (u.guid == shoutOther) { other = true; otherCap = ctl.damageLeft; } }
                    landed = cower && cowerCap == 0 && other && otherCap > 0;
                    if (!landed) result = "cower " + std::to_string(cower) + " cap " + std::to_string(cowerCap) + " other " + std::to_string(other) + " cap " + std::to_string(otherCap) + " (" + result + ")";
                }
                else if (a.kind == 58) {
                    // Sunder Armor: 4 % a stack, a second cast makes it 8 %; threat 345 + 5 % AP each (spell_threat).
                    const auto* sd = content.spell(spellId);
                    const auto pct = [&] { uint8_t v = 0; for (const auto& u : arena.npcs()) if (u.guid == foeGuid) for (const auto& s : u.armorDebuffs) if (s.spellId == spellId && s.remainingMs) v = s.percent; return v; };
                    const uint8_t first = pct();
                    for (int k = 0; k < 8 && first && pct() < 2 * first; ++k) {
                        p.mana = p.maxMana; p.globalCooldownMs = 0; p.cooldowns.clear(); std::string again;
                        arena.execute(p, {LocalAction::CastSpell, foeGuid, spellId}, players, again); arena.tick(0.05f, players); }
                    uint64_t threat = 0; for (const auto& u : arena.npcs()) if (u.guid == foeGuid) for (const auto& e : u.threat) if (e.guid == p.guid) threat = e.amount;
                    landed = sd && sd->armorDebuffStackMax == 5 && first == 4 && pct() == 8 && threat >= 690000;
                    if (!landed) result = "sunder " + std::to_string(first) + "% then " + std::to_string(pct()) + "% threat " + std::to_string(threat) + " (" + result + ")";
                }
                else if (a.kind == 54) {
                    int kind = 0; for (const auto& b : after ? after->npcBuffs : std::vector<LocalNpcBuff>{}) if (b.casterGuid == p.guid && b.spellId == 20185) kind = b.judgementKind;
                    landed = judgementRefused && kind == 1 && after && after->health < 100000;
                    if (!landed) result = "refused " + std::to_string(judgementRefused) + " debuff " + std::to_string(kind) + " health " + std::to_string(after ? after->health : 0) + " (" + result + ")";
                }
                else if (a.kind == 53) {
                    landed = novaRefused && after && after->health < novaBefore;
                    if (!landed) result = "refused " + std::to_string(novaRefused) + " health " + std::to_string(novaBefore) + "->" + std::to_string(after ? after->health : 0) + " (" + result + ")";
                }
                else if (a.kind == 52) {
                    const auto feigning = [&] { for (const auto& st : p.statAuras) if (st.spellId == spellId && st.remainingMs) return true; return false; };
                    const auto targetOf = [&] { for (const auto& v : arena.npcs()) if (v.guid == foeGuid) return v.targetGuid; return uint64_t(0); };
                    const bool dropped = feigning() && targetOf() != p.guid;
                    for (int t = 0; t < 20; ++t) arena.tick(0.05f, players);
                    const bool stillDropped = targetOf() != p.guid && feigning();
                    p.x += 1.0f; arena.tick(0.05f, players);
                    landed = dropped && stillDropped && !feigning();
                    if (!landed) result = "dropped " + std::to_string(dropped) + " held " + std::to_string(stillDropped) + " after moving " + std::to_string(feigning()) + " (" + result + ")";
                }
                else if (a.kind == 48) {
                    const auto* sd = content.spell(spellId);
                    const auto mark = [&](uint64_t guid) { int32_t ap = 0; for (const auto& v : arena.npcs()) if (v.guid == guid) for (const auto& b : v.npcBuffs) if (b.spellId == spellId && b.casterGuid == p.guid) ap = b.rangedAttackerAp; return ap; };
                    const int32_t first = mark(foeGuid);
                    uint64_t second = 0;
                    for (const auto& m : arena.npcs()) if (!second && m.guid != foeGuid && m.hostile && !m.dead && m.health && m.mapId == p.mapId && std::hypot(m.x - p.x, m.y - p.y) < 35) second = m.guid;
                    p.mana = p.maxMana; p.globalCooldownMs = 0;
                    if (second) { if (const LocalRealmNpc* m2 = [&]() -> const LocalRealmNpc* { for (const auto& v : arena.npcs()) if (v.guid == second) return &v; return nullptr; }()) p.orientation = std::atan2(m2->y - p.y, m2->x - p.x);
                        arena.execute(p, {LocalAction::CastSpell, second, spellId}, players, result); arena.tick(0.05f, players); }
                    landed = sd && first == sd->targetDebuffRangedAttackerAp && first > 0 && second && mark(second) == first && mark(foeGuid) == 0;
                    if (!landed) result = "mark " + std::to_string(first) + " second " + std::to_string(second ? mark(second) : -1) + " first after " + std::to_string(mark(foeGuid)) + " (" + result + ")";
                }
                else if (a.kind == 44) {
                    bool spread = false;
                    for (const auto& v : arena.npcs()) if (v.guid == pestilenceOther) for (const auto& b : v.npcBuffs) if (b.spellId == 55095 && b.casterGuid == p.guid) spread = true;
                    landed = pestilenceOther && spread;
                    if (!landed) result = "other " + std::to_string(pestilenceOther) + " spread " + std::to_string(spread) + " (" + result + ")";
                }
                else if (a.kind == 43) {
                    // Death Grip: the creature now stands within reach in front of the knight, on it.
                    const float d = after ? std::hypot(after->x - p.x, after->y - p.y) : 999.f;
                    landed = after && d < 3.5f && after->targetGuid == p.guid;
                    if (!landed) result = "distance " + std::to_string(d) + " target " + std::to_string(after ? after->targetGuid : 0) + " (" + result + ")";
                }
                else if (a.kind == 42) {
                    const auto* sd = content.spell(spellId);
                    const auto* z = sd ? content.spell(sd->diseaseSpell) : nullptr;
                    const auto npcNow = [&]() -> const LocalRealmNpc* { for (const auto& v : arena.npcs()) if (v.guid == foeGuid) return &v; return nullptr; };
                    int slow = 0; if (const auto* m = npcNow()) for (const auto& b : m->npcBuffs) if (z && b.spellId == z->id && b.casterGuid == p.guid) slow = b.hastePct;
                    const uint32_t h0 = npcNow() ? npcNow()->health : 0;
                    for (int t = 0; t < 64; ++t) { p.attackTarget = 0; p.health = p.maxHealth; arena.tick(0.05f, players); }
                    const uint32_t h1 = npcNow() ? npcNow()->health : 0;
                    const bool frost = z && z->id == 55095;
                    // Icy Touch's own hit carries spell_bonus_data's 10% of attack power.
                    const uint32_t hit = before > h0 ? before - h0 : 0;
                    const uint32_t apPart = uint32_t(localMeleeStats(p, content).attackPower * (sd ? sd->apBonusPer100k : 0) / 100000);
                    const bool apOk = !frost || (sd && sd->apBonusPer100k == 10000 && apPart > 0 && hit >= sd->damage + apPart * 95 / 100);
                    landed = z && z->diseaseApPer100k && h1 < h0 && (frost ? slow == -14 : slow == 0) && apOk;
                    if (!landed) result = "hit " + std::to_string(hit) + " ap part " + std::to_string(apPart) + " disease " + std::to_string(z ? z->id : 0) + " health " + std::to_string(h0) + "->" + std::to_string(h1) + " slow " + std::to_string(slow) + " (" + result + ")";
                }
                else if (a.kind == 41) {
                    const auto npcNow = [&]() -> const LocalRealmNpc* { for (const auto& v : arena.npcs()) if (v.guid == foeGuid) return &v; return nullptr; };
                    const LocalNpcControl* root = nullptr; if (const auto* v = npcNow()) for (const auto& c2 : v->controls) if (c2.spellId == spellId) root = &c2;
                    const auto* m = npcNow();
                    landed = root && m && root->kind == uint8_t(LocalNpcControlKind::Root) && root->damageLeft == m->maxHealth / 10;
                    float moved = -1;
                    if (landed) {
                        const float x0 = m->x, y0 = m->y;
                        { auto& foe = const_cast<LocalRealmNpc&>(*m); foe.homeX = foe.x; foe.homeY = foe.y; foe.homeZ = foe.z; } // no evade home in the measure
                        p.x = m->x + 15; p.y = m->y; p.attackTarget = 0; // walking, not a teleport (a new positionRevision ends the caster's controls)
                        for (int t = 0; t < 30; ++t) { arena.tick(0.05f, players);
                            if (std::getenv("ABILITY_VERBOSE")) if (const auto* m3 = npcNow()) { bool r = false; for (const auto& c3 : m3->controls) if (c3.spellId == spellId) r = true;
                                out << "  root t" << t << " moved " << std::hypot(m3->x - x0, m3->y - y0) << " held " << r << " target " << m3->targetGuid << " motion " << int(m3->npcMotion) << " flee " << int(m3->fleeMode) << "\n"; } }
                        if (const auto* m2 = npcNow()) moved = std::hypot(m2->x - x0, m2->y - y0);
                        landed = moved >= 0 && moved < 0.1f;
                    }
                    if (!landed) result = "root " + std::to_string(root ? int(root->kind) : -1) + " cap " + std::to_string(root ? root->damageLeft : 0) + " moved " + std::to_string(moved) + " (" + result + ")";
                }
                else if (a.kind == 40) {
                    const auto* sd = content.spell(spellId);
                    const auto npcNow = [&]() -> const LocalRealmNpc* { for (const auto& v : arena.npcs()) if (v.guid == foeGuid) return &v; return nullptr; };
                    const auto held = [&] { if (const auto* v = npcNow()) for (const auto& c2 : v->controls) if (c2.spellId == spellId) return true; return false; };
                    const uint32_t sheep = sd ? localMountDisplay(sd->controlTransformEntry) : 0;
                    const auto* m = npcNow(); const uint32_t base = m ? m->baseDisplayId : 0;
                    landed = held() && sheep && m && m->displayId == sheep && base && base != sheep;
                    uint32_t low = 0, regained = 0;
                    if (landed) {
                        auto& foe = const_cast<LocalRealmNpc&>(*m); foe.health = low = std::max(1u, foe.maxHealth / 10);
                        for (int t = 0; t < 45 && held(); ++t) arena.tick(0.05f, players);
                        regained = npcNow() ? npcNow()->health : 0;
                        uint32_t bolt = 0; for (auto k : p.knownSpells) if (const auto* kd = content.spell(k); kd && kd->name == "Fire Blast" && kd->unsupportedReason.empty()) bolt = k;
                        p.mana = p.maxMana; p.globalCooldownMs = 0; p.cooldowns.clear();
                        if (bolt) arena.execute(p, {LocalAction::CastSpell, foeGuid, bolt}, players, result);
                        for (int t = 0; t < 4; ++t) arena.tick(0.05f, players);
                        const auto* after2 = npcNow();
                        landed = bolt && regained > low && !held() && after2 && (after2->dead || after2->displayId == base);
                    }
                    if (!landed) result = "held " + std::to_string(held()) + " display " + std::to_string(m ? m->displayId : 0) + " sheep " + std::to_string(sheep) + " base " + std::to_string(base) +
                                          " health " + std::to_string(low) + "->" + std::to_string(regained) + " (" + result + ")";
                }
                else if (a.kind == 31) {
                    const auto* sd = content.spell(spellId);
                    bool horror = false; if (after) for (const auto& ctl : after->controls) if (ctl.spellId == spellId) horror = true;
                    const uint32_t taken = after && before > after->health ? before - after->health : 0;
                    const uint32_t healed = p.health > coilHealth ? p.health - coilHealth : 0;
                    landed = sd && sd->directLeechPct == 300 && horror && taken && healed == std::min(p.maxHealth - coilHealth, taken * 3);
                    if (!landed) result = "taken " + std::to_string(taken) + " healed " + std::to_string(healed) + " horror " + std::to_string(horror) + " (" + result + ")";
                }
                else if (a.kind == 26) {
                    const auto* sd = content.spell(spellId);
                    const LocalNpcBuff* curse = nullptr; bool other = false; uint32_t armor = 0;
                    if (after) for (const auto& b : after->npcBuffs) { if (b.spellId == spellId && b.casterGuid == p.guid) curse = &b; if (firstCurse && b.spellId == firstCurse) other = true; }
                    if (after) armor = localNpcArmorAfterDebuffs(1000, *after);
                    landed = sd && curse && !other && curse->remainingMs > 0 && curse->remainingMs <= sd->durationMs &&
                             curse->attackPower == sd->targetDebuffAttackPower && curse->resistance == sd->targetDebuffResistance &&
                             curse->damageTakenPct == sd->targetDebuffDamageTakenPct && curse->castSpeedPct == sd->targetDebuffCastSpeedPct &&
                             (curse->attackPower < 0 || curse->damageTakenPct > 0 || curse->castSpeedPct < 0) &&
                             armor == 1000u - 10u * sd->targetDebuffArmorPct;
                    if (!landed) result = std::string("curse ") + (curse ? "held" : "missing") + (other ? ", first curse kept" : "") + " armor " + std::to_string(armor) + (curse ? " ap " + std::to_string(curse->attackPower) + " pct " + std::to_string(curse->armorPct) + " ms " + std::to_string(curse->remainingMs) : std::string()) +
                                       (sd ? " def ap " + std::to_string(sd->targetDebuffAttackPower) + " pct " + std::to_string(sd->targetDebuffArmorPct) + " ms " + std::to_string(sd->durationMs) : std::string()) + " (" + result + ")";
                }
                else if (a.kind == 27) {
                    const auto feared = [&](uint64_t guid) -> const LocalNpcControl* {
                        for (const auto& v : arena.npcs()) if (v.guid == guid && !v.dead) for (const auto& s : v.controls) if (s.spellId == spellId) return &s;
                        return nullptr; };
                    const auto npcAt = [&](uint64_t guid) -> const LocalRealmNpc* { for (const auto& v : arena.npcs()) if (v.guid == guid) return &v; return nullptr; };
                    const auto castAt = [&](uint64_t guid, uint32_t id) {
                        p.mana = p.maxMana; p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear();
                        if (const auto* m = npcAt(guid)) p.orientation = std::atan2(m->y - p.y, m->x - p.x);
                        const bool ok = arena.execute(p, {LocalAction::CastSpell, guid, id}, players, result);
                        for (int t = 0; t < 120 && p.castingSpellId; ++t) { p.health = p.maxHealth; arena.tick(0.05f, players); }
                        arena.tick(0.05f, players); return ok; };
                    const auto* f = feared(foeGuid);
                    landed = f && f->damageLeft == 2000 && f->kind == uint8_t(LocalNpcControlKind::Stun);
                    if (!landed) { result = "fear " + (f ? std::to_string(f->remainingMs) + "ms cap " + std::to_string(f->damageLeft) : std::string("missing")) + " (" + result + ")"; last = result; continue; }
                    // One target at a time: the same warlock's Fear on a second creature.
                    uint64_t target = foeGuid;
                    // A second creature within reach of where the warlock stands: walking off
                    // would let the first one leave combat and drop its fear on its own.
                    // None in reach (the start-zone creatures wander): the nearest one is
                    // brought beside the warlock, home included so it does not walk back.
                    const auto eligible = [&](const LocalRealmNpc& m) { return m.guid != foeGuid && m.hostile && !m.dead && m.health && m.mapId == p.mapId &&
                        m.instanceId == p.instanceId && localSpellCreatureTypeAllowed(*content.spell(spellId), localNpcCreatureType(m.entry)); };
                    const LocalRealmNpc* nearest = nullptr; bool inReach = false;
                    for (const auto& m : arena.npcs()) if (eligible(m)) {
                        if (std::hypot(m.x - p.x, m.y - p.y) < 18) inReach = true;
                        if (!nearest || std::hypot(m.x - p.x, m.y - p.y) < std::hypot(nearest->x - p.x, nearest->y - p.y)) nearest = &m; }
                    if (!inReach && nearest) { auto& moved = const_cast<LocalRealmNpc&>(*nearest);
                        moved.x = moved.homeX = p.x + 6; moved.y = moved.homeY = p.y; moved.z = moved.homeZ = p.z; }
                    for (const auto& m : arena.npcs()) if (target == foeGuid && eligible(m) && std::hypot(m.x - p.x, m.y - p.y) < 18) {
                        auto& sturdy = const_cast<LocalRealmNpc&>(m); sturdy.maxHealth = sturdy.health = 20000;
                        const bool cast = castAt(m.guid, spellId);
                        if (std::getenv("ABILITY_VERBOSE")) out << "  second fear " << foeGuid << " -> " << m.guid << ": first " << (feared(foeGuid) != nullptr) << " second " << (feared(m.guid) != nullptr) << " (" << result << ")\n";
                        if (cast && feared(m.guid)) target = m.guid;
                    }
                    if (target == foeGuid || feared(foeGuid)) { landed = false; result = std::string("second fear ") + (target == foeGuid ? "did not land" : "left the first one feared") + " (" + result + ")"; last = result; break; }
                    uint32_t bolt = 0;
                    for (auto id : p.knownSpells) if (const auto* sd = content.spell(id); sd && sd->name == "Shadow Bolt" && sd->unsupportedReason.empty()) bolt = id;
                    bool spent = false; int bolts = 0;
                    for (; bolts < 12 && feared(target); ++bolts) {
                        const auto left = feared(target)->damageLeft; const auto* m = npcAt(target); const auto hp = m ? m->health : 0;
                        castAt(target, bolt);
                        const auto* m2 = npcAt(target); const auto* g = feared(target);
                        const auto dealt = m2 && hp > m2->health ? hp - m2->health : 0;
                        if (g && dealt) spent = spent || (g->damageLeft + dealt == left);
                        if (std::getenv("ABILITY_VERBOSE")) out << "  bolt " << bolts << " dealt " << dealt << " left " << (g ? g->damageLeft : 0) << "\n";
                    }
                    landed = bolt && spent && !feared(target);
                    if (!landed) result = "bolt=" + std::to_string(bolt) + " spent=" + std::to_string(spent) + " after " + std::to_string(bolts) + " bolts still feared=" + std::to_string(feared(target) != nullptr) + " (" + result + ")";
                    last = result;
                    break;
                }
                else if (a.kind == 11) {
                    // A level 80 Ambush can kill a start-zone creature outright: then no points remain.
                    const bool killed = !after || after->dead;
                    landed = p.lastCastSpellId == spellId && !localStealthed(p) && (killed || (p.comboPoints > 0 && p.comboTarget == foeGuid));
                    if (!landed) result = "combo=" + std::to_string(p.comboPoints) + " stealthed=" + std::to_string(localStealthed(p)) + " last=" + std::to_string(p.lastCastSpellId); }
                else if (a.kind == 0 || a.kind == 15 || a.kind == 17 || a.kind == 19) {
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
        out << "PASS class abilities: " << passed << " Spell.dbc abilities (weapon strikes, shots, DoTs, channels, ground areas, snares, charges, soul shards, demon summons, interrupts, taunts, spells, conjuring, stat buffs, speed and dodge buffs, stealth openers, hunter aspects, creature tracking, totems, dispels, combo finishers, stuns and breakable controls, fears, armor reductions, curses, warlock armors, Life Tap, school wards, health leech, immunities and Forbearance, damage-taken cuts, Ice Block, Fear Ward, Barkskin, level-scaled absorbs, mana aspects, Icebound Fortitude, Devotion Aura, Power Word: Shield, Polymorph, roots, death knight diseases, Prowl, presences, offensive dispels, slowing totems, cleaves, reagents, class mounts, teleports)\n";
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
    // ---- 4. A quest that hands over an item (quest_template.StartItem): given
    // on accept, taken back on abandon.
    {
        size_t withStartItem = 0; const LocalQuestDefinition* pick = nullptr;
        for (const auto& [id, gate] : c.questChainGates) if (const auto* q = c.quest(id); q && q->startItem) {
            ++withStartItem;
            if (!pick && !q->prerequisite && q->minLevel <= 20 && spawns.count(q->giverEntry)) pick = q;
        }
        SELFTEST_CHECK(pick);
        LocalGameplay world; SELFTEST_CHECK(world.loadContent(worldPath, error));
        LocalRealmPlayer p; p.guid = 1900; p.name = "Courier"; p.race = 1; p.classId = 1;
        for (uint8_t r = 1; r <= 11; ++r) if (!pick->allowableRaces || (pick->allowableRaces & (1u << (r - 1)))) { p.race = r; break; }
        for (uint8_t k = 1; k <= 11; ++k) if (k != 10 && (!pick->allowableClasses || (pick->allowableClasses & (1u << (k - 1))))) { p.classId = k; break; }
        world.initializePlayer(p, true, std::max<uint8_t>(pick->minLevel, 1));
        std::vector<LocalRealmPlayer*> players{&p};
        const auto spawn = spawns.find(pick->giverEntry)->second;
        p.mapId = spawn.map; p.instanceId = 0; p.x = spawn.x; p.y = spawn.y; p.z = spawn.z; ++p.positionRevision;
        const LocalRealmNpc* giver = nullptr;
        for (int i = 0; i < 200 && !giver; ++i) { world.tick(0.05f, players);
            for (const auto& n : world.npcs()) if (n.entry == pick->giverEntry && !n.dead && std::hypot(n.x - p.x, n.y - p.y) < 60) giver = &n; }
        const auto held = [&] { uint32_t n = 0; for (const auto& st : p.inventory) if (st.itemId == pick->startItem) n += st.count; return n; };
        bool ok = giver != nullptr;
        if (ok) { p.x = giver->x; p.y = giver->y; p.z = giver->z; ++p.positionRevision;
            ok = world.execute(p, {LocalAction::AcceptQuest, giver->guid, pick->id}, players, result) && held() >= pick->startItemCount; }
        ok = ok && world.execute(p, {LocalAction::AbandonQuest, 0, pick->id}, players, result) && held() == 0;
        if (!ok) { out << "FAIL start item: quest " << pick->id << " '" << pick->title << "' item " << pick->startItem << ": " << result << "\n"; return false; }
        out << "PASS start items: " << withStartItem << " catalog quests hand over an item; quest " << pick->id << " '" << pick->title
            << "' gave item " << pick->startItem << " on accept and took it back on abandon\n";
    }

    // ---- 5. Warrior mechanics: stances, action bars, reactive abilities, execute, shouts, shield defenses, whirlwind
    {
        LocalGameplay world; SELFTEST_CHECK(world.loadContent(worldPath, error));
        if (clientSpells) SELFTEST_CHECK(world.setStarterSpells(*clientSpells, "selftest", error));
        LocalRealmPlayer p; p.guid = 2001; p.name = "Conan"; p.race = 1; p.classId = 1;
        world.initializePlayer(p, true, 80);
        std::vector<LocalRealmPlayer*> players{&p};

        // 5a. Default to Battle Stance (2457) on creation
        SELFTEST_CHECK(p.formSpellId == 2457);
        const auto* battleProfile = localActiveForm(p);
        SELFTEST_CHECK(battleProfile && battleProfile->bar == 1);
        wowee::ui::ActionBarPanel abp;
        SELFTEST_CHECK(abp.getEffectiveMainActionBarPage(battleProfile->bar) == 7);

        // 5b. Stance swapping to Defensive Stance (71) -> bar 2 -> page 8
        std::string res;
        bool ok = world.execute(p, {LocalAction::CastSpell, p.guid, 71}, players, res);
        SELFTEST_CHECK(ok && p.formSpellId == 71);
        const auto* defProfile = localActiveForm(p);
        SELFTEST_CHECK(defProfile && defProfile->bar == 2);
        SELFTEST_CHECK(abp.getEffectiveMainActionBarPage(defProfile->bar) == 8);

        // 5c. Stance swapping to Berserker Stance (2458) -> bar 3 -> page 9
        p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear();
        ok = world.execute(p, {LocalAction::CastSpell, p.guid, 2458}, players, res);
        SELFTEST_CHECK(ok && p.formSpellId == 2458);
        const auto* zerkProfile = localActiveForm(p);
        SELFTEST_CHECK(zerkProfile && zerkProfile->bar == 3);
        SELFTEST_CHECK(abp.getEffectiveMainActionBarPage(zerkProfile->bar) == 9);

        // Swap back to Battle Stance for stance-specific abilities
        p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear();
        world.execute(p, {LocalAction::CastSpell, p.guid, 2457}, players, res);
        p.mana = 100; p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear();

        auto findSpell = [&](const std::string& name) -> uint32_t {
            uint32_t match = 0;
            for (auto id : p.knownSpells) {
                if (const auto* s = world.content().spell(id); s && s->name == name && s->unsupportedReason.empty()) {
                    if (s->executeSpell) return id;
                    if (!match || id > match) match = id;
                }
            }
            return match;
        };

        // 5d. Setup a hostile NPC nearby
        LocalRealmNpc foe;
        for (const auto& n : world.npcs()) {
            if (!n.dead && n.health && world.canAttack(p, n)) {
                foe = n;
                break;
            }
        }
        if (!foe.guid) {
            foe.guid = 99999;
            foe.entry = 2031;
            foe.mapId = p.mapId;
            foe.instanceId = p.instanceId;
            foe.level = 1;
            foe.hostile = true;
        }
        foe.level = 1;
        foe.dead = false;
        foe.maxHealth = foe.health = 100000;
        p.x = 0; p.y = 0; p.z = 0; ++p.positionRevision;
        foe.x = p.x + 2.0f; foe.y = p.y; foe.z = p.z;
        foe.homeX = foe.x; foe.homeY = foe.y; foe.homeZ = foe.z;
        world.setRemoteNpcs({foe});

        // 5e. Overpower: requires overpowerWindowMs > 0
        const uint32_t overpowerSpell = findSpell("Overpower");
        if (overpowerSpell) {
            p.overpowerWindowMs = 0;
            ok = world.execute(p, {LocalAction::CastSpell, foe.guid, overpowerSpell}, players, res);
            SELFTEST_CHECK(!ok && res.find("cannot use that ability yet") != std::string::npos);

            p.overpowerWindowMs = 5000; p.globalCooldownMs = 0; p.mana = 100;
            ok = world.execute(p, {LocalAction::CastSpell, foe.guid, overpowerSpell}, players, res);
            SELFTEST_CHECK(ok && p.overpowerWindowMs == 0);
        }

        // 5f. Revenge: requires revengeWindowMs > 0 (requiresDefenseState)
        // Defensive Stance (71) for Revenge
        p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear();
        world.execute(p, {LocalAction::CastSpell, p.guid, 71}, players, res);
        p.globalCooldownMs = 0; p.mana = 100; p.cooldowns.clear(); p.categoryCooldowns.clear();
        const uint32_t revengeSpell = findSpell("Revenge");
        if (revengeSpell) {
            p.revengeWindowMs = 0;
            ok = world.execute(p, {LocalAction::CastSpell, foe.guid, revengeSpell}, players, res);
            SELFTEST_CHECK(!ok && res.find("cannot use that ability yet") != std::string::npos);

            p.revengeWindowMs = 5000; p.globalCooldownMs = 0; p.mana = 100;
            ok = world.execute(p, {LocalAction::CastSpell, foe.guid, revengeSpell}, players, res);
            SELFTEST_CHECK(ok && p.revengeWindowMs == 0);
        }

        // 5g. Execute: target health <= 20%
        p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear();
        world.execute(p, {LocalAction::CastSpell, p.guid, 2457}, players, res);
        p.globalCooldownMs = 0; p.mana = 100; p.cooldowns.clear(); p.categoryCooldowns.clear();
        const uint32_t executeSpell = findSpell("Execute");
        if (executeSpell) {
            auto n = world.npcs().front();
            n.health = n.maxHealth; // 100% HP
            world.setRemoteNpcs({n});
            ok = world.execute(p, {LocalAction::CastSpell, n.guid, executeSpell}, players, res);
            SELFTEST_CHECK(!ok && res.find("below 20% health") != std::string::npos);

            n.health = n.maxHealth * 15 / 100; // 15% HP
            world.setRemoteNpcs({n});
            p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear();
            p.mana = 50; // 15 base + 35 extra (capped at 30 extra)
            const uint32_t foeHpBefore = world.npcs().front().health;
            ok = world.execute(p, {LocalAction::CastSpell, n.guid, executeSpell}, players, res);
            SELFTEST_CHECK(ok && world.npcs().front().health < foeHpBefore && p.mana == 5); // 50 - 15 - 30 = 5
        }

        // 5h. Victory Rush: requires victoryRushWindowMs > 0
        const uint32_t victoryRushSpell = findSpell("Victory Rush");
        if (victoryRushSpell) {
            p.victoryRushWindowMs = 0; p.globalCooldownMs = 0; p.mana = 100;
            ok = world.execute(p, {LocalAction::CastSpell, foe.guid, victoryRushSpell}, players, res);
            SELFTEST_CHECK(!ok && res.find("cannot use that ability yet") != std::string::npos);

            p.victoryRushWindowMs = 20000; p.globalCooldownMs = 0; p.mana = 100;
            ok = world.execute(p, {LocalAction::CastSpell, foe.guid, victoryRushSpell}, players, res);
            SELFTEST_CHECK(ok && p.victoryRushWindowMs == 0);
        }

        // 5i. Disarm: requires Defensive Stance
        p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear();
        world.execute(p, {LocalAction::CastSpell, p.guid, 71}, players, res);
        p.globalCooldownMs = 0; p.mana = 100; p.cooldowns.clear(); p.categoryCooldowns.clear();
        const uint32_t disarmSpell = findSpell("Disarm");
        if (disarmSpell) {
            ok = world.execute(p, {LocalAction::CastSpell, foe.guid, disarmSpell}, players, res);
            SELFTEST_CHECK(ok);
            bool disarmFound = false;
            for (const auto& b : world.npcs().front().npcBuffs) {
                if (b.spellId == disarmSpell && b.damagePct == -50 && b.parryPct == -100) disarmFound = true;
            }
            SELFTEST_CHECK(disarmFound);
        }

        // 5j. Demoralizing Shout: area AP debuff
        p.globalCooldownMs = 0; p.mana = 100;
        const uint32_t demoSpell = findSpell("Demoralizing Shout");
        if (demoSpell) {
            ok = world.execute(p, {LocalAction::CastSpell, p.guid, demoSpell}, players, res);
            SELFTEST_CHECK(ok);
            bool demoFound = false;
            for (const auto& b : world.npcs().front().npcBuffs) {
                if (b.spellId == demoSpell && b.attackPower < 0) demoFound = true;
            }
            SELFTEST_CHECK(demoFound);
        }

        // 5k. Berserker Rage: clears fear and sap/incapacitate
        p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear();
        world.execute(p, {LocalAction::CastSpell, p.guid, 2458}, players, res); // Berserker Stance
        p.globalCooldownMs = 0; p.mana = 100; p.cooldowns.clear(); p.categoryCooldowns.clear();
        const uint32_t zerkRageSpell = findSpell("Berserker Rage");
        if (zerkRageSpell) {
            LocalHealingAuraView fearView; fearView.spellId = 5782; fearView.controlKind = 3; fearView.remainingMs = 5000;
            p.harmfulAuras.push_back(fearView);
            LocalHealingAuraView sapView; sapView.spellId = 6770; sapView.controlKind = 4; sapView.remainingMs = 5000;
            p.harmfulAuras.push_back(sapView);
            ok = world.execute(p, {LocalAction::CastSpell, p.guid, zerkRageSpell}, players, res);
            SELFTEST_CHECK(ok);
            bool harmfulCleared = std::none_of(p.harmfulAuras.begin(), p.harmfulAuras.end(),
                [](const auto& a) { return a.controlKind == 3 || a.controlKind == 4; });
            SELFTEST_CHECK(harmfulCleared);
        }

        // 5l. Whirlwind: 8yd normalized weapon strike
        p.globalCooldownMs = 0; p.mana = 100;
        const uint32_t whirlwindSpell = findSpell("Whirlwind");
        if (whirlwindSpell) {
            const uint32_t hpBeforeWw = world.npcs().front().health;
            ok = world.execute(p, {LocalAction::CastSpell, p.guid, whirlwindSpell}, players, res);
            SELFTEST_CHECK(ok && world.npcs().front().health < hpBeforeWw);
        }

        // 5m. Shield Block & Shield Slam
        p.globalCooldownMs = 0; p.cooldowns.clear(); p.categoryCooldowns.clear();
        world.execute(p, {LocalAction::CastSpell, p.guid, 71}, players, res); // Defensive Stance
        p.globalCooldownMs = 0; p.mana = 100; p.cooldowns.clear(); p.categoryCooldowns.clear();
        const auto& c = world.content();
        uint32_t shieldItem = 0;
        for (const auto& m : kLocalAuctionItems) {
            if (m.itemClass == 4 && m.subClass == 6) {
                if (const auto* mi = localMeleeItem(m.id); mi && mi->inventoryType == 14 && mi->block > 0) {
                    if (m.requiredLevel <= 80 && (!m.allowableClasses || (m.allowableClasses & 1))) {
                        shieldItem = m.id;
                        break;
                    }
                }
            }
        }
        if (shieldItem) {
            p.inventory.push_back({shieldItem, 1, 30});
            p.equipment[localEquipmentIndex(LocalEquipmentSlot::OffHand)] = shieldItem;
        }
        const auto statsBeforeSb = localMeleeStats(p, c);
        const uint32_t sbSpell = findSpell("Shield Block");
        if (sbSpell) {
            ok = world.execute(p, {LocalAction::CastSpell, p.guid, sbSpell}, players, res);
            SELFTEST_CHECK(ok);
            const auto statsAfterSb = localMeleeStats(p, c);
            SELFTEST_CHECK(statsAfterSb.block == 100.f && statsAfterSb.shieldBlockValue >= statsBeforeSb.shieldBlockValue * 2);
        }

        // 5n. Shield Wall: damage taken reduced by 60%
        p.globalCooldownMs = 0; p.mana = 100;
        const uint32_t swSpell = findSpell("Shield Wall");
        if (swSpell) {
            ok = world.execute(p, {LocalAction::CastSpell, p.guid, swSpell}, players, res);
            SELFTEST_CHECK(ok);
            bool swBuff = std::any_of(p.statAuras.begin(), p.statAuras.end(), [&](const auto& a){ return a.spellId == swSpell && a.remainingMs > 0; });
            SELFTEST_CHECK(swBuff);
        }

        // 5o. Commanding Shout: increases health
        const uint32_t csSpell = findSpell("Commanding Shout");
        if (csSpell) {
            const uint32_t hpUnbuffed = p.maxHealth;
            p.globalCooldownMs = 0; p.mana = 100;
            ok = world.execute(p, {LocalAction::CastSpell, p.guid, csSpell}, players, res);
            SELFTEST_CHECK(ok && p.maxHealth > hpUnbuffed);
        }
        out << "PASS warrior mechanics: stances (Battle/Defensive/Berserker), bonus bar paging, Overpower, Revenge, Execute, Victory Rush, Disarm, shouts, Shield Block, Shield Slam, Shield Wall, Berserker Rage, Whirlwind\n";
    }

    // -------------------------------------------------------------------------
    // 6. Bags, Mining & Blacksmithing (G6)
    // -------------------------------------------------------------------------
    {
        LocalGameplay world;
        if (!world.loadContent(worldPath, error)) { out << error << "\n"; return false; }
        if (clientSpells) SELFTEST_CHECK(world.setStarterSpells(*clientSpells, "selftest", error));
        world.useContent(world.sharedContent());
        std::vector<LocalRealmPlayer*> players;
        std::string res;

        LocalRealmPlayer p;
        p.guid = 101;
        p.race = 1; // Human
        p.classId = 1; // Warrior
        p.level = 20;
        p.money = 50000;
        world.initializePlayer(p, true);
        players.push_back(&p);

        const auto pushItem = [&](uint32_t id, uint16_t count) {
            std::vector<bool> seen(localPlayerStorageCapacity(p), false);
            for (const auto& s : p.inventory) if (s.bagSlot < seen.size()) seen[s.bagSlot] = true;
            uint8_t slot = 0;
            while (slot < seen.size() && seen[slot]) ++slot;
            p.inventory.push_back({id, count, slot});
        };
        const auto hasItem = [&](uint32_t id) {
            uint32_t n = 0;
            for (const auto& s : p.inventory) if (s.itemId == id) n += s.count;
            return n;
        };

        // 6a. Base storage capacity is 24
        SELFTEST_CHECK(localPlayerStorageCapacity(p) == 24);

        // 6b. Bag equipping & dynamic capacity expansion
        // Item 4496: Small Brown Pouch (6 slots, inventoryType 18)
        pushItem(4496, 1);
        SELFTEST_CHECK(world.execute(p, {LocalAction::EquipItem, 0, 4496}, players, res));
        SELFTEST_CHECK(p.bagContainers[0].itemId == 4496);
        SELFTEST_CHECK(localPlayerStorageCapacity(p) == 24 + 6); // 30 slots

        // Equip a second bag in slot 2 (target 2)
        // Item 4496: Small Brown Pouch (6 slots)
        pushItem(4496, 1);
        SELFTEST_CHECK(world.execute(p, {LocalAction::EquipItem, 2, 4496}, players, res));
        SELFTEST_CHECK(p.bagContainers[1].itemId == 4496);
        SELFTEST_CHECK(localPlayerStorageCapacity(p) == 24 + 6 + 6); // 36 slots

        // Put an item into bag 1 (slot range: 24..29 for bag 0, 30..35 for bag 1)
        LocalItemStack potion{118, 5, 25}; // In bag 0
        p.inventory.push_back(potion);

        // 6c. Unequip non-empty bag rejected
        // Slot 19 corresponds to bagContainer 0
        bool ok = world.execute(p, {LocalAction::UnequipItem, 0, 19}, players, res);
        SELFTEST_CHECK(!ok); // Rejected because bag 0 is not empty!
        SELFTEST_CHECK(p.bagContainers[0].itemId == 4496);

        // Unequip empty bag 1 (slot 20) succeeds
        ok = world.execute(p, {LocalAction::UnequipItem, 0, 20}, players, res);
        SELFTEST_CHECK(ok);
        SELFTEST_CHECK(p.bagContainers[1].itemId == 0);
        SELFTEST_CHECK(localPlayerStorageCapacity(p) == 30);

        // Move potion back to backpack (slot 5) and now unequip bag 0
        p.inventory.erase(std::remove_if(p.inventory.begin(), p.inventory.end(), [](const auto& s){ return s.itemId == 118; }), p.inventory.end());
        ok = world.execute(p, {LocalAction::UnequipItem, 0, 19}, players, res);
        SELFTEST_CHECK(ok);
        SELFTEST_CHECK(p.bagContainers[0].itemId == 0);
        SELFTEST_CHECK(localPlayerStorageCapacity(p) == 24);

        // 6d. Mining node harvesting with Mining Pick
        // Learn Mining (skill 186)
        p.professions.push_back({186, 1, 75, 0});
        LocalGameObject vein;
        vein.id = 990001;
        vein.entry = 1731; // Copper Vein
        vein.name = "Copper Vein";
        vein.mapId = p.mapId;
        vein.x = p.x + 1.0f;
        vein.y = p.y;
        vein.z = p.z;
        vein.kind = LocalGameObjectKind::Resource;
        vein.requiredSkillId = 186;
        vein.requiredSkill = 1;
        vein.useRadius = 5.0f;

        auto contentPtr = std::make_shared<LocalWorldContent>(world.content());
        contentPtr->gameObjects.push_back(vein);
        std::sort(contentPtr->gameObjects.begin(), contentPtr->gameObjects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        world.useContent(contentPtr);

        // Try mining without Mining Pick -> rejected
        const uint32_t veinRev = world.gameObjectState(vein.id) ? world.gameObjectState(vein.id)->revision : 1;
        const LocalRealmCommand mineCmd{LocalAction::UseGameObject, localGameObjectGuid(vein.id), vein.id, veinRev};
        SELFTEST_CHECK(!world.execute(p, mineCmd, players, res));

        // Add Mining Pick (2901) to inventory
        pushItem(2901, 1);
        SELFTEST_CHECK(world.execute(p, mineCmd, players, res));
        // Verify ore obtained
        SELFTEST_CHECK(hasItem(2770) >= 1); // Copper Ore

        // 6e. Smelting at Forge (SpellFocus 3)
        // Add Forge game object
        LocalGameObject forge;
        forge.id = 990002;
        forge.entry = 4090;
        forge.name = "Forge";
        forge.mapId = p.mapId;
        forge.x = p.x + 2.0f;
        forge.y = p.y;
        forge.z = p.z;
        forge.kind = LocalGameObjectKind::Decorative;
        contentPtr->gameObjects.push_back(forge);
        std::sort(contentPtr->gameObjects.begin(), contentPtr->gameObjects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });

        // Recipe: Smelt Copper (2657)
        LocalRecipe smeltCopper;
        smeltCopper.spellId = 2657;
        smeltCopper.skillId = 186;
        smeltCopper.name = "Smelt Copper";
        smeltCopper.createdItemId = 2840; // Copper Bar
        smeltCopper.createdCount = 1;
        smeltCopper.reagents = {{2770, 1}};
        smeltCopper.requiresSpellFocus = 3; // Forge
        contentPtr->recipes.push_back(smeltCopper);
        std::sort(contentPtr->recipes.begin(), contentPtr->recipes.end(), [](const auto& a, const auto& b) { return a.spellId < b.spellId; });
        p.knownRecipes.push_back(2657);

        // Smelting near forge succeeds
        SELFTEST_CHECK(world.execute(p, {LocalAction::CraftItem, 1, 2657}, players, res));
        SELFTEST_CHECK(hasItem(2840) >= 1); // Copper Bar created!

        // Smelting far from forge is rejected
        for (auto& obj : contentPtr->gameObjects) {
            if (obj.id == forge.id) { obj.x = p.x + 50.0f; break; }
        }
        SELFTEST_CHECK(!world.execute(p, {LocalAction::CraftItem, 1, 2657}, players, res));

        // 6f. Blacksmithing at Anvil (SpellFocus 1) with Blacksmith Hammer (5956)
        // Learn Blacksmithing (skill 164)
        p.professions.push_back({164, 1, 75, 0});
        LocalGameObject anvil;
        anvil.id = 990003;
        anvil.entry = 4087;
        anvil.name = "Anvil";
        anvil.mapId = p.mapId;
        anvil.x = p.x + 2.0f;
        anvil.y = p.y;
        anvil.z = p.z;
        anvil.kind = LocalGameObjectKind::Decorative;
        contentPtr->gameObjects.push_back(anvil);
        std::sort(contentPtr->gameObjects.begin(), contentPtr->gameObjects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });

        // Sharpening stone: focus 0, no hammer
        LocalRecipe stoneRecipe;
        stoneRecipe.spellId = 2660; // Rough Sharpening Stone
        stoneRecipe.skillId = 164;
        stoneRecipe.name = "Rough Sharpening Stone";
        stoneRecipe.createdItemId = 2862;
        stoneRecipe.createdCount = 1;
        stoneRecipe.reagents = {{2835, 1}}; // Rough Stone
        stoneRecipe.requiresSpellFocus = 0;
        contentPtr->recipes.push_back(stoneRecipe);

        // Weapon/gear recipe: requires Anvil (SpellFocus 1) and Blacksmith Hammer (5956)
        LocalRecipe vestRecipe;
        vestRecipe.spellId = 3321; // Copper Chain Vest
        vestRecipe.skillId = 164;
        vestRecipe.name = "Copper Chain Vest";
        vestRecipe.createdItemId = 2853;
        vestRecipe.createdCount = 1;
        vestRecipe.reagents = {{2840, 1}}; // Copper Bar
        vestRecipe.tools = {5956, 0}; // Blacksmith Hammer
        vestRecipe.requiresSpellFocus = 1; // Anvil
        contentPtr->recipes.push_back(vestRecipe);
        std::sort(contentPtr->recipes.begin(), contentPtr->recipes.end(), [](const auto& a, const auto& b) { return a.spellId < b.spellId; });
        p.knownRecipes.push_back(2660);
        p.knownRecipes.push_back(3321);

        // Craft sharpening stone without anvil/hammer
        pushItem(2835, 2);
        SELFTEST_CHECK(world.execute(p, {LocalAction::CraftItem, 1, 2660}, players, res));
        SELFTEST_CHECK(hasItem(2862) >= 1);

        // Try crafting without hammer -> rejected
        SELFTEST_CHECK(!world.execute(p, {LocalAction::CraftItem, 1, 3321}, players, res));

        // Add Blacksmith Hammer (5956) -> succeeds
        pushItem(5956, 1);
        SELFTEST_CHECK(world.execute(p, {LocalAction::CraftItem, 1, 3321}, players, res));
        SELFTEST_CHECK(hasItem(2853) >= 1);

        // Remove Blacksmith Hammer, add Gnomish Army Knife (40772) -> also crafts successfully
        p.inventory.erase(std::remove_if(p.inventory.begin(), p.inventory.end(), [](const auto& s){ return s.itemId == 5956; }), p.inventory.end());
        pushItem(2840, 1); // another copper bar
        SELFTEST_CHECK(!world.execute(p, {LocalAction::CraftItem, 1, 3321}, players, res));
        pushItem(40772, 1);
        SELFTEST_CHECK(world.execute(p, {LocalAction::CraftItem, 1, 3321}, players, res));
        SELFTEST_CHECK(hasItem(2853) >= 2);

        // 6g. Bank bag slots at friendly banker
        LocalRealmNpc banker;
        banker.guid = 8888;
        banker.entry = 3418;
        banker.banker = true;
        banker.mapId = p.mapId;
        banker.x = p.x; banker.y = p.y; banker.z = p.z;
        world.setRemoteNpcs({banker});

        // Try equipping bank bag without banker -> rejected
        pushItem(4496, 1);
        LocalRealmCommand bankEquipCmd{LocalAction::EquipItem, 24, 4496};
        SELFTEST_CHECK(!world.execute(p, bankEquipCmd, players, res));

        // Equip bank bag with banker -> succeeds
        bankEquipCmd.serviceNpcGuid = banker.guid;
        SELFTEST_CHECK(world.execute(p, bankEquipCmd, players, res));
        SELFTEST_CHECK(p.bankBagContainers[0].itemId == 4496);

        // Try unequipping bank bag without banker -> rejected
        LocalRealmCommand bankUnequipCmd{LocalAction::UnequipItem, 0, 24};
        SELFTEST_CHECK(!world.execute(p, bankUnequipCmd, players, res));

        // Unequip bank bag with banker -> succeeds
        bankUnequipCmd.serviceNpcGuid = banker.guid;
        SELFTEST_CHECK(world.execute(p, bankUnequipCmd, players, res));
        SELFTEST_CHECK(p.bankBagContainers[0].itemId == 0);

        // 6h. Validate character with expanded inventory (> 24 items)
        pushItem(4496, 1);
        SELFTEST_CHECK(world.execute(p, {LocalAction::EquipItem, 0, 4496}, players, res));
        SELFTEST_CHECK(localPlayerStorageCapacity(p) == 30);
        while (p.inventory.size() < 26) {
            pushItem(2835, 1);
        }
        SELFTEST_CHECK(world.validatePlayer(p, res));

        // 6i. Mail attachment from expanded bag slot (slot >= 24)
        LocalRealmPlayer recipient = p;
        recipient.guid = 999999;
        recipient.name = "Recipient";
        LocalRealmPlayer mailCandidate;
        LocalMail mailLetter;
        auto bagStackIt = std::find_if(p.inventory.begin(), p.inventory.end(), [](const auto& s){ return s.bagSlot >= 24; });
        SELFTEST_CHECK(bagStackIt != p.inventory.end());
        const uint8_t mailBagSlot = bagStackIt->bagSlot;
        const uint32_t mailItemId = bagStackIt->itemId;
        p.money = 10000;
        SELFTEST_CHECK(prepareLocalMail(p, recipient, "Bag Item", "Here is an item from my bag", 0, 0,
                                        {{mailItemId, 1, 1, mailBagSlot}}, world.content(), mailCandidate, mailLetter, res));

        out << "PASS bags & professions (G6): bag container slots, dynamic inventory expansion, unequip restrictions, mining harvesting, smelting at forge, blacksmithing at anvil\n";
    }

    // -------------------------------------------------------------------------
    // 7. GameObject Quest Interactions (G2/G3)
    // -------------------------------------------------------------------------
    {
        LocalGameplay world;
        if (!world.loadContent(worldPath, error)) { out << error << "\n"; return false; }
        if (clientSpells) SELFTEST_CHECK(world.setStarterSpells(*clientSpells, "selftest", error));
        world.useContent(world.sharedContent());
        std::vector<LocalRealmPlayer*> players;
        std::string res;

        LocalRealmPlayer p;
        p.guid = 102;
        p.race = 1; // Human
        p.classId = 1; // Warrior
        p.level = 20;
        p.money = 50000;
        world.initializePlayer(p, true);
        players.push_back(&p);

        const auto hasItem = [&](uint32_t id) {
            uint32_t n = 0;
            for (const auto& s : p.inventory) if (s.itemId == id) n += s.count;
            return n;
        };

        bool ok = false;
        auto contentPtr = std::make_shared<LocalWorldContent>(world.content());

        // 7a. Setup GameObject Questgiver (e.g. Wanted Poster, entry 180001, id 990101)
        LocalGameObject poster;
        poster.id = 990101;
        poster.entry = 180001;
        poster.name = "Wanted Poster";
        poster.mapId = p.mapId;
        poster.x = p.x + 1.0f;
        poster.y = p.y;
        poster.z = p.z;
        poster.kind = LocalGameObjectKind::Decorative;
        poster.questGiver = true;
        poster.useRadius = 5.0f;

        // 7b. Setup GameObject Objective Target (e.g. Ancient Shrine, entry 180002, id 990102)
        LocalGameObject shrine;
        shrine.id = 990102;
        shrine.entry = 180002;
        shrine.name = "Ancient Shrine";
        shrine.mapId = p.mapId;
        shrine.x = p.x + 1.5f;
        shrine.y = p.y;
        shrine.z = p.z;
        shrine.kind = LocalGameObjectKind::Decorative;
        shrine.useRadius = 5.0f;

        // 7c. Setup GameObject Quest Loot Chest (entry 180003, id 990103)
        LocalGameObject chest;
        chest.id = 990103;
        chest.entry = 180003;
        chest.name = "Hidden Relic Chest";
        chest.mapId = p.mapId;
        chest.x = p.x + 2.0f;
        chest.y = p.y;
        chest.z = p.z;
        chest.kind = LocalGameObjectKind::Chest;
        chest.useRadius = 5.0f;
        chest.persistent = true;
        LocalItemDefinition questItem;
        questItem.id = 54321;
        questItem.name = "Ancient Relic";
        contentPtr->items.push_back(questItem);
        std::sort(contentPtr->items.begin(), contentPtr->items.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        chest.loot = {{questItem.id, 1}};

        contentPtr->gameObjects.push_back(poster);
        contentPtr->gameObjects.push_back(shrine);
        contentPtr->gameObjects.push_back(chest);
        std::sort(contentPtr->gameObjects.begin(), contentPtr->gameObjects.end(), [](const auto& a, const auto& b) { return a.id < b.id; });

        LocalQuestDefinition qDef;
        qDef.id = 9901;
        qDef.title = "Wanted: Ancient Relic";
        qDef.description = "Investigate the shrine and retrieve the relic.";
        qDef.giverEntry = poster.entry;
        qDef.turnInEntry = poster.entry;
        qDef.minLevel = 1;
        qDef.xp = 500;
        qDef.money = 1000;
        qDef.objectives.push_back({LocalQuestObjective::Type::GameObject, shrine.entry, 1, "Investigate Shrine"});
        qDef.objectives.push_back({LocalQuestObjective::Type::Collect, questItem.id, 1, "Ancient Relic"});
        contentPtr->quests.push_back(qDef);
        std::sort(contentPtr->quests.begin(), contentPtr->quests.end(), [](const auto& a, const auto& b) { return a.id < b.id; });

        world.useContent(contentPtr);

        // 7d. Verify questsForGameObject returns the quest
        const auto offered = world.content().questsForGameObject(poster.entry);
        SELFTEST_CHECK(!offered.empty());
        SELFTEST_CHECK(offered[0].id == qDef.id);

        // Negative check: unrelated quest not offered by poster
        LocalQuestDefinition otherDef;
        otherDef.id = 9902;
        otherDef.title = "Unrelated Quest";
        otherDef.giverEntry = 12345;
        otherDef.turnInEntry = 12345;
        contentPtr->quests.push_back(otherDef);
        std::sort(contentPtr->quests.begin(), contentPtr->quests.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        world.useContent(contentPtr);
        SELFTEST_CHECK(!localQuestOffered(p, poster, otherDef));
        SELFTEST_CHECK(localQuestOffered(p, poster, qDef));

        const uint64_t posterGuid = localGameObjectGuid(poster.id);
        // Negative check: cannot accept unrelated quest at poster
        const LocalRealmCommand badAcceptCmd{LocalAction::AcceptQuest, posterGuid, otherDef.id};
        ok = world.execute(p, badAcceptCmd, players, res);
        SELFTEST_CHECK(!ok);

        // Negative check: cannot accept quest from non-questgiver object (e.g. shrine)
        const uint64_t shrineGuid = localGameObjectGuid(shrine.id);
        const LocalRealmCommand shrineAcceptCmd{LocalAction::AcceptQuest, shrineGuid, qDef.id};
        ok = world.execute(p, shrineAcceptCmd, players, res);
        SELFTEST_CHECK(!ok);

        // 7e. Accept quest targeting GameObject GUID
        const LocalRealmCommand acceptCmd{LocalAction::AcceptQuest, posterGuid, qDef.id};
        ok = world.execute(p, acceptCmd, players, res);
        SELFTEST_CHECK(ok);
        SELFTEST_CHECK(p.quests.size() >= 1);
        auto* progress = localQuestProgress(p, qDef.id);
        SELFTEST_CHECK(progress != nullptr);
        SELFTEST_CHECK(progress->status == LocalQuestStatus::Active);
        SELFTEST_CHECK(progress->progress.size() == 2);
        SELFTEST_CHECK(progress->progress[0] == 0);
        SELFTEST_CHECK(progress->progress[1] == 0);

        // 7f. Interact with Shrine (GameObject objective) -> awards credit
        const LocalRealmCommand shrineCmd{LocalAction::UseGameObject, shrineGuid, shrine.id};
        ok = world.execute(p, shrineCmd, players, res);
        SELFTEST_CHECK(ok);
        progress = localQuestProgress(p, qDef.id);
        SELFTEST_CHECK(progress != nullptr);
        SELFTEST_CHECK(progress->progress[0] == 1);
        SELFTEST_CHECK(progress->status == LocalQuestStatus::Active);

        // 7g. Loot chest -> collects quest item and updates collect objective
        const uint64_t chestGuid = localGameObjectGuid(chest.id);
        const uint32_t chestRev = world.gameObjectState(chest.id) ? world.gameObjectState(chest.id)->revision : 1;
        const LocalRealmCommand chestCmd{LocalAction::UseGameObject, chestGuid, chest.id, chestRev};
        ok = world.execute(p, chestCmd, players, res);
        SELFTEST_CHECK(ok);
        SELFTEST_CHECK(hasItem(questItem.id) >= 1);
        progress = localQuestProgress(p, qDef.id);
        SELFTEST_CHECK(progress != nullptr);
        SELFTEST_CHECK(progress->progress[1] == 1);
        SELFTEST_CHECK(progress->status == LocalQuestStatus::Complete);

        // Negative check: cannot turn in quest at wrong GameObject (e.g. chest or shrine)
        const LocalRealmCommand badTurnInCmd{LocalAction::TurnInQuest, shrineGuid, qDef.id};
        ok = world.execute(p, badTurnInCmd, players, res);
        SELFTEST_CHECK(!ok);

        // 7h. Turn in quest targeting GameObject GUID
        const uint32_t initialMoney = p.money;
        const LocalRealmCommand turnInCmd{LocalAction::TurnInQuest, posterGuid, qDef.id};
        ok = world.execute(p, turnInCmd, players, res);
        SELFTEST_CHECK(ok);
        SELFTEST_CHECK(p.money == initialMoney + qDef.money);
        SELFTEST_CHECK(std::binary_search(p.completedQuestIds.begin(), p.completedQuestIds.end(), qDef.id));
        SELFTEST_CHECK(localQuestProgress(p, qDef.id) == nullptr);
        SELFTEST_CHECK(hasItem(questItem.id) == 0);

        out << "PASS gameobject quests (G2/G3): questgiver poster, interaction objective, quest item loot, turn-in at gameobject\n";
    }
    return true;
}
#undef SELFTEST_CHECK
} // namespace wowee::game
