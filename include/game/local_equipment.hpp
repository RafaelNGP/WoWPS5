#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace wowee::game {

// Save v5 and LAN wire v4 use the WotLK equipment order. Bag containers are
// separate from these nineteen worn slots.
enum class LocalEquipmentSlot : uint8_t {
    Head, Neck, Shoulders, Shirt, Chest, Waist, Legs, Feet, Wrists, Hands,
    Finger1, Finger2, Trinket1, Trinket2, Back, MainHand, OffHand, Ranged, Tabard
};
inline constexpr size_t kLocalEquipmentSlotCount = 19;
inline constexpr size_t localEquipmentIndex(LocalEquipmentSlot slot) { return static_cast<size_t>(slot); }
inline constexpr std::array<size_t, 4> kLegacyLocalEquipmentSlots = {15, 4, 6, 7};
inline constexpr std::array<const char*, kLocalEquipmentSlotCount> kLocalEquipmentSlotNames = {
    "Head", "Neck", "Shoulders", "Shirt", "Chest", "Waist", "Legs", "Feet", "Wrists", "Hands",
    "Finger 1", "Finger 2", "Trinket 1", "Trinket 2", "Back", "Main hand", "Off hand", "Ranged", "Tabard"
};

inline constexpr uint32_t localEquipmentSlotBit(size_t slot) {
    return slot < kLocalEquipmentSlotCount ? uint32_t(1) << slot : 0;
}

// inventoryType is authoritative whenever present. Legacy schema-1 fixtures
// without it retain their explicit 1=weapon, 2=chest, 3=legs, 4=feet meaning.
// A real non-wearable type must never fall back to a legacy adapted slot.
inline constexpr uint32_t localEquipmentSlotMask(uint8_t inventoryType, uint8_t legacySlot = 0) {
    switch (inventoryType) {
        case 1: return localEquipmentSlotBit(0);
        case 2: return localEquipmentSlotBit(1);
        case 3: return localEquipmentSlotBit(2);
        case 4: return localEquipmentSlotBit(3);
        case 5: case 20: return localEquipmentSlotBit(4);
        case 6: return localEquipmentSlotBit(5);
        case 7: return localEquipmentSlotBit(6);
        case 8: return localEquipmentSlotBit(7);
        case 9: return localEquipmentSlotBit(8);
        case 10: return localEquipmentSlotBit(9);
        case 11: return localEquipmentSlotBit(10) | localEquipmentSlotBit(11);
        case 12: return localEquipmentSlotBit(12) | localEquipmentSlotBit(13);
        case 13: return localEquipmentSlotBit(15) | localEquipmentSlotBit(16);
        case 14: case 22: case 23: return localEquipmentSlotBit(16);
        case 15: case 25: case 26: case 28: return localEquipmentSlotBit(17);
        case 16: return localEquipmentSlotBit(14);
        case 17: case 21: return localEquipmentSlotBit(15);
        case 19: return localEquipmentSlotBit(18);
        case 0: return legacySlot >= 1 && legacySlot <= kLegacyLocalEquipmentSlots.size()
            ? localEquipmentSlotBit(kLegacyLocalEquipmentSlots[legacySlot - 1]) : 0;
        default: return 0; // Bags, ammo, quivers and unknown inventory types.
    }
}

inline constexpr bool localEquipmentFits(uint8_t inventoryType, uint8_t legacySlot, size_t slot) {
    return (localEquipmentSlotMask(inventoryType, legacySlot) & localEquipmentSlotBit(slot)) != 0;
}

// WotLK class proficiencies (item_template class 2 weapons, class 4 armor),
// i.e. what the class trainers and weapon masters can teach. Mail waits for
// level 40 on hunters and shamans, plate for warriors and paladins; death
// knights start with plate. Class ids: 1 Warrior, 2 Paladin, 3 Hunter,
// 4 Rogue, 5 Priest, 6 Death Knight, 7 Shaman, 8 Mage, 9 Warlock, 11 Druid.
inline constexpr bool localClassCanUseItem(uint8_t classId, uint8_t level, uint8_t itemClass, uint8_t subClass) {
    constexpr auto bit = [](uint8_t n) { return uint32_t(1) << n; };
    if (itemClass == 2) {
        // 0 axe, 1 2h axe, 2 bow, 3 gun, 4 mace, 5 2h mace, 6 polearm, 7 sword,
        // 8 2h sword, 10 staff, 13 fist, 14 misc, 15 dagger, 16 thrown,
        // 18 crossbow, 19 wand, 20 fishing pole.
        uint32_t mask = bit(14) | bit(20);
        switch (classId) {
            case 1: mask |= bit(0)|bit(1)|bit(2)|bit(3)|bit(4)|bit(5)|bit(6)|bit(7)|bit(8)|bit(10)|bit(13)|bit(15)|bit(16)|bit(18); break;
            case 2: case 6: mask |= bit(0)|bit(1)|bit(4)|bit(5)|bit(6)|bit(7)|bit(8); break;
            case 3: mask |= bit(0)|bit(1)|bit(2)|bit(3)|bit(6)|bit(7)|bit(8)|bit(10)|bit(13)|bit(15)|bit(16)|bit(18); break;
            case 4: mask |= bit(0)|bit(2)|bit(3)|bit(4)|bit(7)|bit(13)|bit(15)|bit(16)|bit(18); break;
            case 5: mask |= bit(4)|bit(10)|bit(15)|bit(19); break;
            case 7: mask |= bit(0)|bit(1)|bit(4)|bit(5)|bit(10)|bit(13)|bit(15); break;
            case 8: case 9: mask |= bit(7)|bit(10)|bit(15)|bit(19); break;
            case 11: mask |= bit(4)|bit(5)|bit(6)|bit(10)|bit(13)|bit(15); break;
            default: return false;
        }
        return subClass < 32 && (mask & bit(subClass)) != 0;
    }
    if (itemClass == 4) {
        // 0 misc (rings, trinkets), 1 cloth, 2 leather, 3 mail, 4 plate,
        // 6 shield, 7 libram, 8 idol, 9 totem, 10 sigil.
        switch (subClass) {
            case 0: case 1: return true;
            case 2: return classId != 5 && classId != 8 && classId != 9;
            case 3: return classId == 1 || classId == 2 || classId == 6 || ((classId == 3 || classId == 7) && level >= 40);
            case 4: return classId == 6 || ((classId == 1 || classId == 2) && level >= 40);
            case 6: return classId == 1 || classId == 2 || classId == 7;
            case 7: return classId == 2;
            case 8: return classId == 11;
            case 9: return classId == 7;
            case 10: return classId == 6;
            default: return false;
        }
    }
    return true;
}

} // namespace wowee::game
