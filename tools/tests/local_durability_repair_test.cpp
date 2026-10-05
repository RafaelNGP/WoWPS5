#include "game/local_gameplay.hpp"
#include "game/local_melee.hpp"
#include "game/local_ranged.hpp"
#include "game/local_spell_equipment.hpp"
#include "local_group_rewards_fixture.hpp"
#include <iostream>
#include <cassert>

using namespace wowee::game;

static LocalRealmPlayer makePlayer(uint64_t guid) {
    auto p = rewardPlayer(guid);
    p.race = 1;      // Human
    p.classId = 1;   // Warrior
    p.level = 80;
    p.money = 100000; // 10 gold
    p.inventory.clear();
    p.equipment.fill(0);
    return p;
}

static LocalRealmNpc makeRepairVendor(uint64_t guid = 100) {
    LocalRealmNpc n = rewardNpc(guid);
    n.entry = 54;
    n.name = "Blacksmith";
    n.hostile = false;
    n.repairer = true;
    n.vendor = true;
    n.x = 0; n.y = 0; n.z = 0;
    return n;
}

static LocalItemStack* findEquippedStack(LocalRealmPlayer& p, size_t slot) {
    if (slot >= p.equipment.size()) return nullptr;
    const auto id = p.equipment[slot];
    if (!id) return nullptr;
    const auto copies = std::count(p.equipment.begin(), p.equipment.begin() + slot + 1, id);
    size_t currentCopy = 0;
    for (auto& s : p.inventory) {
        if (s.itemId == id) {
            if (currentCopy + s.count >= uint32_t(copies)) {
                return &s;
            }
            currentCopy += s.count;
        }
    }
    return nullptr;
}

int main() {
    std::cout << "[TEST] Loading real world.json and durability.json...\n";
    LocalGameplay game;
    std::string err;
    assert(game.loadContent("assets/local_realm/world.json", err));
    const auto& c = game.content();

    // 1. Durability lookup verification
    {
        std::cout << "[TEST] 1. Catalog durability lookup...\n";
        assert(!c.itemDurability.empty());
        for (size_t i = 1; i < c.itemDurability.size(); ++i) {
            assert(c.itemDurability[i - 1].itemId < c.itemDurability[i].itemId);
            assert(c.itemDurability[i].maxDurability > 0);
        }

        // Item 25: [25, 20, 800] -> Worn Shortsword, maxDur 20, 800 millicopper/pt
        const auto* dur25 = c.durability(25);
        assert(dur25 != nullptr);
        assert(dur25->itemId == 25);
        assert(dur25->maxDurability == 20);
        assert(dur25->costPerPoint == 800);

        // Item 200: [200, 60, 4000] -> Thorg's Bone, maxDur 60, 4000 millicopper/pt
        const auto* dur200 = c.durability(200);
        assert(dur200 != nullptr);
        assert(dur200->itemId == 200);
        assert(dur200->maxDurability == 60);
        assert(dur200->costPerPoint == 4000);

        // Non-durable or non-existent items
        assert(c.durability(99999999) == nullptr);
        std::cout << "  PASS 1: Durability catalog loaded and verified\n";
    }

    // 2. Player item initialization & durability migration
    {
        std::cout << "[TEST] 2. Player item initialization & migration...\n";
        auto p = makePlayer(2);
        // Item 25 with 0 maxDurability should be migrated by initializePlayer
        p.inventory.push_back({25, 1, 0, {}});
        p.inventory.push_back({200, 1, 1, {}});
        p.inventory.push_back({117, 5, 2, {}}); // Reward item without durability
        game.initializePlayer(p, false);

        assert(p.inventory[0].instance.maxDurability == 20);
        assert(p.inventory[0].instance.curDurability == 20);
        assert(p.inventory[1].instance.maxDurability == 60);
        assert(p.inventory[1].instance.curDurability == 60);
        assert(p.inventory[2].instance.maxDurability == 0);
        assert(p.inventory[2].instance.curDurability == 0);
        std::cout << "  PASS 2: Item durability correctly initialized on migration\n";
    }

    // 3. Broken items zero stats, armor, and weapon damage
    {
        std::cout << "[TEST] 3. Broken items zero stats, armor, and weapon damage...\n";
        auto p = makePlayer(3);
        // Find a piece of armor in catalog with durability and armor
        uint32_t armorId = 0;
        size_t armorSlot1Based = 0;
        for (const auto& item : c.items) {
            if (item.armor > 0 && c.durability(item.id)) {
                const uint32_t mask = localEquipmentSlotMask(item.inventoryType, item.slot);
                for (size_t s = 0; s < kLocalEquipmentSlotCount; ++s) {
                    if (mask & localEquipmentSlotBit(s)) {
                        armorId = item.id;
                        armorSlot1Based = s + 1;
                        break;
                    }
                }
                if (armorSlot1Based) break;
            }
        }
        assert(armorId > 0 && armorSlot1Based > 0);
        p.inventory.push_back({armorId, 1, 0, {}});
        p.inventory.push_back({25, 1, 1, {}}); // Weapon (Worn Shortsword)
        game.initializePlayer(p, false);

        std::vector<LocalRealmPlayer*> players{&p};
        std::string res;
        // Equip armor
        assert(game.execute(p, {LocalAction::EquipItem, armorSlot1Based, armorId}, players, res));
        // Equip sword in main hand (slot 16 is MainHand, 1-based)
        assert(game.execute(p, {LocalAction::EquipItem, 16, 25}, players, res));

        const uint32_t fullArmor = localMeleeArmor(p, c);
        assert(worn(p, c, 15) != nullptr);

        // Break the armor (curDurability = 0)
        auto* armorStack = findEquippedStack(p, armorSlot1Based - 1);
        assert(armorStack != nullptr);
        armorStack->instance.curDurability = 0;

        const uint32_t brokenArmor = localMeleeArmor(p, c);
        assert(brokenArmor < fullArmor);

        // Break the weapon
        auto* swordStack = findEquippedStack(p, 15);
        assert(swordStack != nullptr);
        swordStack->instance.curDurability = 0;
        assert(worn(p, c, 15) == nullptr);

        // Weapon-requiring ability rejects when weapon is broken
        LocalSpellDefinition weaponSpell;
        weaponSpell.id = 99991;
        weaponSpell.requiresMainHand = true;
        assert(!localSpellEquipmentReady(p, c, weaponSpell));

        // Restore weapon durability
        swordStack->instance.curDurability = swordStack->instance.maxDurability;
        assert(worn(p, c, 15) != nullptr);
        assert(localSpellEquipmentReady(p, c, weaponSpell));

        // Ranged weapon test
        // Item 2504: Worn Shortbow (ranged weapon)
        if (c.durability(2504)) {
            p.inventory.push_back({2504, 1, 2, {}});
            p.inventory.push_back({2512, 200, 3, {}}); // Rough Arrow ammo
            game.initializePlayer(p, false);
            assert(game.execute(p, {LocalAction::EquipItem, 18, 2504}, players, res));
            assert(worn(p, c, 17) != nullptr);

            // Break ranged weapon
            auto* bowStack = findEquippedStack(p, 17);
            assert(bowStack != nullptr);
            bowStack->instance.curDurability = 0;
            assert(worn(p, c, 17) == nullptr);

            // Ranged auto attack cannot fire with broken bow
            LocalSpellDefinition autoShot;
            autoShot.id = 75;
            autoShot.clientSpell = true;
            autoShot.rangedAutoProfile = 1;
            assert(!localRangedAmounts(p, c, autoShot).active);
        }
        std::cout << "  PASS 3: Broken items correctly yield 0 stats and disable attacks\n";
    }

    // 4. Death durability loss (10% on death, 25% on spirit healer)
    {
        std::cout << "[TEST] 4. Death durability loss (10% death, 25% spirit healer)...\n";
        auto p = makePlayer(4);
        p.inventory.push_back({25, 1, 0, {}});  // maxDur 20
        p.inventory.push_back({200, 1, 1, {}}); // maxDur 60
        p.inventory.push_back({35, 1, 2, {}});  // maxDur 25 (in inventory, not equipped)
        game.initializePlayer(p, false);

        std::vector<LocalRealmPlayer*> players{&p};
        std::string res;
        assert(game.execute(p, {LocalAction::EquipItem, 16, 25}, players, res));

        auto* sword = findEquippedStack(p, 15);
        assert(sword && sword->instance.curDurability == 20);

        // Fatal fall damage causes 10% durability reduction on equipped gear
        p.health = 50;
        p.falling = true;
        p.fallStartZ = p.z + 100.0f;
        game.tick(0.05f, players);
        assert(p.dead);
        // 20 * 0.10 = 2 -> 20 - 2 = 18
        assert(sword->instance.curDurability == 18);

        // Unequipped bag item 35 should NOT take ordinary death durability loss
        auto it35 = std::find_if(p.inventory.begin(), p.inventory.end(), [](auto& s){ return s.itemId == 35; });
        assert(it35 != p.inventory.end() && it35->instance.curDurability == 25);

        // Spirit Healer revive: 25% reduction on ALL items
        LocalRealmNpc healer = rewardNpc(200);
        healer.entry = 6491;
        healer.name = "Spirit Healer";
        healer.mapId = p.mapId;
        healer.instanceId = p.instanceId;
        healer.x = p.x; healer.y = p.y; healer.z = p.z;
        game.setRemoteNpcs({healer});

        p.ghost = true;
        p.corpseValid = true;
        p.corpseX = p.x + 100.0f; // Corpse far away so revive is at spirit healer
        p.corpseY = p.y + 100.0f;
        LocalRealmCommand resCmd{LocalAction::ReclaimCorpse};
        assert(game.execute(p, resCmd, players, res));
        assert(!p.dead);
        // Sword was 18, 25% of 20 = 5 -> 18 - 5 = 13
        assert(sword->instance.curDurability == 13);
        // Bag item 35 was 25, 25% of 25 = round(6.25) = 6 -> 25 - 6 = 19
        assert(it35->instance.curDurability == 19);
        std::cout << "  PASS 4: Fatal death and Spirit Healer durability loss verified\n";
    }

    // 5. Equipment repair rules and costs
    {
        std::cout << "[TEST] 5. Equipment repair rules and costs...\n";
        auto p = makePlayer(5);
        p.money = 10000;
        p.inventory.push_back({25, 1, 0, {}});  // maxDur 20, 800 millicopper/pt (0.8 copper/pt)
        p.inventory.push_back({200, 1, 1, {}}); // maxDur 60, 4000 millicopper/pt (4.0 copper/pt)
        game.initializePlayer(p, false);

        std::vector<LocalRealmPlayer*> players{&p};
        std::string res;
        assert(game.execute(p, {LocalAction::EquipItem, 16, 25}, players, res));

        auto repairVendor = makeRepairVendor(100);
        game.setRemoteNpcs({repairVendor});

        // 1. Repair when full: no-op, 0 charged
        LocalRealmCommand repCmd{LocalAction::RepairEquipment};
        repCmd.serviceNpcGuid = repairVendor.guid;
        assert(game.execute(p, repCmd, players, res));
        assert(res.find("all equipment is at full durability") != std::string::npos);
        assert(p.money == 10000);

        // 2. Damage items:
        // Item 25: lost 10 points (20 -> 10). Cost = (10 * 800) / 1000 = 8 copper
        // Item 200: lost 10 points (60 -> 50). Cost = (10 * 4000) / 1000 = 40 copper
        // Total = 48 copper
        auto it25 = std::find_if(p.inventory.begin(), p.inventory.end(), [](auto& s){ return s.itemId == 25; });
        auto it200 = std::find_if(p.inventory.begin(), p.inventory.end(), [](auto& s){ return s.itemId == 200; });
        it25->instance.curDurability = 10;
        it200->instance.curDurability = 50;

        // Repair without repair NPC -> reject
        LocalRealmCommand badNpcCmd{LocalAction::RepairEquipment};
        badNpcCmd.serviceNpcGuid = 999;
        assert(!game.execute(p, badNpcCmd, players, res));
        assert(res.find("Stand at a blacksmith") != std::string::npos);

        // Repair with insufficient money -> reject
        p.money = 20; // Needs 48 copper
        assert(!game.execute(p, repCmd, players, res));
        assert(res.find("cannot afford") != std::string::npos);
        assert(it25->instance.curDurability == 10);
        assert(it200->instance.curDurability == 50);

        // Repair with sufficient money
        p.money = 1000;
        assert(game.execute(p, repCmd, players, res));
        assert(res.find("Repaired equipment for 48 copper") != std::string::npos);
        assert(p.money == 1000 - 48);
        assert(it25->instance.curDurability == 20);
        assert(it200->instance.curDurability == 60);

        // 3. Reputation discount test (20% discount on Exalted vendor)
        it25->instance.curDurability = 10;   // 8 copper
        it200->instance.curDurability = 50;  // 40 copper
        // Total base = 48 copper. 20% discount -> 48 * 8000 / 10000 = 38 copper
        p.money = 1000;
        p.reputations.push_back({72, 42000}); // Exalted with Stormwind

        auto swVendor = makeRepairVendor(101);
        swVendor.entry = 55;
        LocalNpcDefinition vendorDef;
        vendorDef.id = 55;
        vendorDef.name = "Alliance Smith";
        vendorDef.faction = 12; // Maps to faction 72
        vendorDef.npcFlags = kLocalNpcFlagRepair;
        auto modContent = std::make_shared<LocalWorldContent>(c);
        modContent->npcs.push_back(vendorDef);
        std::sort(modContent->npcs.begin(), modContent->npcs.end(), [](auto& a, auto& b){ return a.id < b.id; });
        LocalFactionTemplate templ;
        templ.id = 12;
        templ.faction = 72;
        std::array<uint32_t, 12> races{};
        races[1] = 12;
        std::string msg;
        assert(game.setFactionTemplates({templ}, races, msg));
        game.useContent(modContent);
        game.setRemoteNpcs({swVendor});

        LocalRealmCommand discountRepCmd{LocalAction::RepairEquipment};
        discountRepCmd.serviceNpcGuid = swVendor.guid;
        assert(game.execute(p, discountRepCmd, players, res));
        assert(res.find("Repaired equipment for 38 copper") != std::string::npos);
        assert(p.money == 1000 - 38);
        assert(it25->instance.curDurability == 20);
        assert(it200->instance.curDurability == 60);

        std::cout << "  PASS 5: Repair rules, costs, rejections and reputation discounts verified\n";
    }

    // 6. Single-item repair
    {
        std::cout << "[TEST] 6. Single-item repair via cmd.id...\n";
        auto p = makePlayer(6);
        p.money = 10000;
        p.inventory.push_back({25, 1, 0, {}});  // maxDur 20, 800 millicopper/pt (8 copper)
        p.inventory.push_back({200, 1, 1, {}}); // maxDur 60, 4000 millicopper/pt (40 copper)
        game.initializePlayer(p, false);

        std::vector<LocalRealmPlayer*> players{&p};
        std::string res;
        auto repairVendor = makeRepairVendor(100);
        game.setRemoteNpcs({repairVendor});

        auto it25 = std::find_if(p.inventory.begin(), p.inventory.end(), [](auto& s){ return s.itemId == 25; });
        auto it200 = std::find_if(p.inventory.begin(), p.inventory.end(), [](auto& s){ return s.itemId == 200; });
        it25->instance.curDurability = 10;   // lost 10 -> 8 copper
        it200->instance.curDurability = 50;  // lost 10 -> 40 copper

        // Query repairCost API for single item
        assert(game.repairCost(p, repairVendor.guid, 25) == 8);
        assert(game.repairCost(p, repairVendor.guid, 200) == 40);
        assert(game.repairCost(p, repairVendor.guid) == 48);

        // Repair ONLY item 25
        LocalRealmCommand singleCmd{LocalAction::RepairEquipment, 0, 25};
        singleCmd.serviceNpcGuid = repairVendor.guid;
        assert(game.execute(p, singleCmd, players, res));
        assert(res.find("Repaired equipment for 8 copper") != std::string::npos);
        assert(p.money == 10000 - 8);
        assert(it25->instance.curDurability == 20);
        assert(it200->instance.curDurability == 50); // Item 200 untouched!

        // Remaining repairCost is now only 40
        assert(game.repairCost(p, repairVendor.guid) == 40);

        std::cout << "  PASS 6: Single-item repair verified\n";
    }

    // 7. Bank item durability migration
    {
        std::cout << "[TEST] 7. Bank item durability migration in initializePlayer...\n";
        auto p = makePlayer(7);
        p.bank[0] = {25, 1, 0, {}}; // Worn Shortsword with 0 maxDurability
        p.bank[1] = {200, 1, 1, {}};
        p.bank[2] = {117, 5, 2, {}}; // Non-durable item
        game.initializePlayer(p, false);

        assert(p.bank[0].instance.maxDurability == 20);
        assert(p.bank[0].instance.curDurability == 20);
        assert(p.bank[1].instance.maxDurability == 60);
        assert(p.bank[1].instance.curDurability == 60);
        assert(p.bank[2].instance.maxDurability == 0);
        std::cout << "  PASS 7: Bank item durability migration verified\n";
    }

    // 8. Dual-wield off-hand weapon immunity from armor degradation
    {
        std::cout << "[TEST] 8. Dual-wield off-hand weapon immunity from armor damage...\n";
        auto p = makePlayer(8);
        p.inventory.push_back({25, 1, 0, {}}); // Main-hand weapon
        p.inventory.push_back({25, 1, 1, {}}); // Off-hand weapon
        game.initializePlayer(p, false);

        std::vector<LocalRealmPlayer*> players{&p};
        std::string res;
        assert(game.execute(p, {LocalAction::EquipItem, 16, 25}, players, res)); // Main hand
        assert(game.execute(p, {LocalAction::EquipItem, 17, 25}, players, res)); // Off hand

        auto* oh = findEquippedStack(p, 16);
        assert(oh && oh->instance.curDurability == 20);

        // Attempting to degrade armor when only weapons are worn:
        // No armor pieces exist, and off-hand weapon must NOT be chosen as armor.
        for (uint32_t roll = 0; roll < 20; ++roll) {
            localDamageArmorInCombat(p, roll);
        }
        assert(oh->instance.curDurability == 20); // Off-hand weapon untouched!
        std::cout << "  PASS 8: Off-hand weapon excluded from incoming armor damage\n";
    }

    // 9. Passive proc weapon requirements reject broken weapons
    {
        std::cout << "[TEST] 9. Passive proc weapon requirements reject broken weapons...\n";
        auto p = makePlayer(9);
        p.inventory.push_back({25, 1, 0, {}}); // Worn Shortsword (itemClass 2, subclass 7)
        game.initializePlayer(p, false);

        std::vector<LocalRealmPlayer*> players{&p};
        std::string res;
        assert(game.execute(p, {LocalAction::EquipItem, 16, 25}, players, res));

        auto* sword = findEquippedStack(p, 15);
        assert(sword && sword->instance.curDurability == 20);
        assert(worn(p, c, 15) != nullptr);

        sword->instance.curDurability = 0;
        assert(worn(p, c, 15) == nullptr);
        std::cout << "  PASS 9: Broken weapons return null from worn() for proc validation\n";
    }

    std::cout << "\nALL DURABILITY & REPAIR TESTS PASSED SUCCESSFULLY!\n";
    return 0;
}
