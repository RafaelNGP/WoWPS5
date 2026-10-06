# Warrior — implementation status (standalone / LAN world)

This page tracks what the local ruleset runs for the Warrior. It is measured,
not estimated: the importer was run over a build-12340 DBC snapshot and every
row below is what `tools/local_realm/audit_client_spells.cpp` reports.

## What comes from your own client data

WoWPS5 does not ship Blizzard data. These all come from **your** WotLK 3.3.5a
(12340) MPQs at runtime, and need no per-class work:

| Presentation | Source in your client |
|---|---|
| Ability and talent names, ranks, descriptions | `Spell.dbc` (Name, Rank, Description/Tooltip) |
| Tooltip numbers (`$s1`, `$/10;s1`, `$d`...) | Resolved from `Spell.dbc` by `GameHandler::formatSpellDescription` |
| Ability and talent icons | `SpellIcon.dbc` + `Interface\Icons\*.blp` |
| Talent tree layout, prerequisites, backgrounds | `Talent.dbc`, `TalentTab.dbc` and the original FrameXML talent frame |
| Weapon/armor tooltips (damage, speed, DPS, armor, stats) | Item data read from your client and the original FrameXML tooltip |
| Cast and impact animations, spell visuals | `SpellVisual*.dbc`, character `.m2` animations |

The work for a class is therefore the **rules**: which spells and talents the
local world can actually run, and the formulas it runs them with.

Known tooltip gaps (all classes): `${...}` arithmetic expressions are removed
rather than evaluated, and `$a` (radius), `$t` (period), `$h` (proc chance) and
`$n` (charges) are not resolved yet.

## Abilities (non-talent)

All trainable Warrior abilities decode and run, including Battle, Defensive and
Berserker Stance with their own action bars, Charge, Intercept, Heroic Strike,
Cleave, Rend, Thunder Clap, Hamstring, Overpower, Revenge, Execute, Victory
Rush, Slam, Mortal Strike, Bloodthirst, Whirlwind, Sunder Armor, Shield Bash,
Shield Block, Shield Slam, Shield Wall, Spell Reflection, Disarm, Taunt,
Mocking Blow, Challenging Shout, Intimidating Shout, Battle / Commanding /
Demoralizing Shout, Bloodrage, Berserker Rage, Recklessness, Retaliation,
Pummel, Heroic Throw, Sweeping Strikes and Death Wish.

Still refused by the importer: Intervene, Shattering Throw, Enraged
Regeneration, and the talent-granted Devastate.

### Systems added in this round

- **Next-swing queue.** Heroic Strike and Cleave (`SPELL_ATTR0_ON_NEXT_SWING`)
  are queued and replace the next main-hand swing instead of hitting at once.
  The rage is checked when queued and spent when the swing lands. Pressing again
  cancels; the strike is off the global cooldown; without rage or a target at
  the swing it falls back to a white hit.
- **Thunder Clap's slow.** Its `MOD_MELEE_HASTE` rider (-10% attack speed) is
  applied to every creature the clap hits. It used to be discarded.
- **Mechanic duration reduction** (auras 232/234): stun, charm, snare and disarm
  durations from creatures are shortened by the warrior's talents, after
  diminishing returns.

## Talents — 44 of 85 run (132 of 228 ranks)

Each accepted talent is pinned to its exact 12340 record. If a client's record
differs in any gameplay column, the talent stays blocked instead of running only
part of its effect.

### Arms (15 / 31)

| Tier | Talent | Ranks | Runs |
|---|---|---|---|
| 1 | Improved Heroic Strike | 3 | yes |
| 1 | Deflection | 5 | yes |
| 1 | Improved Rend | 2 | yes |
| 2 | Improved Charge | 2 | yes |
| 2 | Iron Will | 3 | yes |
| 2 | Tactical Mastery | 3 | yes |
| 3 | Improved Overpower | 2 | yes |
| 3 | Anger Management | 1 | no: periodic rage aura (85) |
| 3 | Impale | 2 | no: critical damage modifier |
| 3 | Deep Wounds | 3 | no: proc |
| 4 | Two-Handed Weapon Specialization | 3 | yes |
| 4 | Taste for Blood | 3 | no: proc |
| 5 | Poleaxe Specialization | 5 | no: critical damage (aura 163) |
| 5 | Sweeping Strikes | 1 | no: proc |
| 5 | Mace Specialization | 5 | yes |
| 5 | Sword Specialization | 5 | no: proc |
| 6 | Weapon Mastery | 2 | yes |
| 6 | Improved Hamstring | 3 | no: proc |
| 6 | Trauma | 2 | no: proc |
| 7 | Second Wind | 2 | no: proc |
| 7 | Mortal Strike | 1 | yes |
| 7 | Strength of Arms | 2 | yes |
| 7 | Improved Slam | 2 | yes |
| 8 | Juggernaut | 1 | no: proc |
| 8 | Improved Mortal Strike | 3 | yes |
| 8 | Unrelenting Assault | 2 | yes |
| 9 | Sudden Death | 3 | no: proc |
| 9 | Endless Rage | 1 | no: rage generation (aura 213) |
| 9 | Blood Frenzy | 2 | no: aura 109 |
| 10 | Wrecking Crew | 5 | no: proc |
| 11 | Bladestorm | 1 | no: periodic trigger (aura 23) |

### Fury (18 / 27)

| Tier | Talent | Ranks | Runs |
|---|---|---|---|
| 1 | Armored to the Teeth | 3 | yes |
| 1 | Booming Voice | 2 | yes |
| 1 | Cruelty | 5 | yes |
| 2 | Improved Demoralizing Shout | 5 | yes |
| 2 | Unbridled Wrath | 5 | yes |
| 3 | Improved Cleave | 3 | yes |
| 3 | Piercing Howl | 1 | no: area snare |
| 3 | Blood Craze | 3 | yes |
| 3 | Commanding Presence | 5 | no: shout amounts are not snapshotted per caster |
| 4 | Dual Wield Specialization | 5 | yes |
| 4 | Improved Execute | 2 | yes |
| 4 | Enrage | 5 | no: proc |
| 5 | Precision | 3 | yes |
| 5 | Death Wish | 1 | yes |
| 5 | Improved Intercept | 2 | yes |
| 6 | Improved Berserker Rage | 2 | no: proc |
| 6 | Flurry | 5 | yes |
| 7 | Intensify Rage | 3 | yes |
| 7 | Bloodthirst | 1 | yes |
| 7 | Improved Whirlwind | 2 | yes |
| 8 | Furious Attacks | 2 | no: proc |
| 8 | Improved Berserker Stance | 5 | no: stance-conditional stats |
| 9 | Heroic Fury | 1 | yes |
| 9 | Rampage | 1 | no: owner-bound area aura |
| 9 | Bloodsurge | 3 | no: proc |
| 10 | Unending Fury | 5 | yes |
| 11 | Titan's Grip | 1 | no: effect 155 |

### Protection (11 / 27)

| Tier | Talent | Ranks | Runs |
|---|---|---|---|
| 1 | Improved Bloodrage | 2 | yes |
| 1 | Shield Specialization | 5 | no: proc |
| 1 | Improved Thunder Clap | 3 | yes |
| 2 | Incite | 3 | yes |
| 2 | Anticipation | 5 | yes |
| 3 | Last Stand | 1 | no: effect 3 |
| 3 | Improved Revenge | 2 | no: proc |
| 3 | Shield Mastery | 2 | no: aura 150 |
| 3 | Toughness | 5 | yes |
| 4 | Improved Spell Reflection | 2 | no: aura 4 |
| 4 | Improved Disarm | 2 | yes |
| 4 | Puncture | 3 | yes |
| 5 | Improved Disciplines | 2 | yes |
| 5 | Concussion Blow | 1 | no: control beside damage |
| 5 | Gag Order | 2 | no: proc |
| 6 | One-Handed Weapon Specialization | 5 | yes |
| 7 | Improved Defensive Stance | 2 | no: proc |
| 7 | Vigilance | 1 | no: proc |
| 7 | Focused Rage | 3 | yes |
| 8 | Vitality | 3 | yes |
| 8 | Safeguard | 2 | no: proc |
| 9 | Warbringer | 1 | no: aura 262 |
| 9 | Devastate | 1 | no: repeated weapon damage |
| 9 | Critical Block | 3 | no: aura 253 |
| 10 | Sword and Board | 3 | no: proc |
| 10 | Damage Shield | 2 | no: proc |
| 11 | Shockwave | 1 | no: area targeting |

## Formulas the local world uses (AzerothCore reference)

- **Melee attack power**: `3 × level + 2 × Strength − 20`, plus gear, buffs and
  Armored to the Teeth (`armor / 108, 54, 36`).
- **Melee critical chance**: class base + Agility × `gtChanceToMeleeCrit` ratio +
  crit rating, plus Cruelty (weapon-qualified), Berserker Stance (+3%) and
  Recklessness charges; Improved Overpower adds to Overpower only.
- **Dodge / parry**: class base + diminishing returns on Agility, defense and
  rating; Anticipation and Deflection add 1% per rank undiminished.
- **Expertise**: rating points + talent points (Vitality, Strength of Arms), each
  point removing 0.25% from the target's dodge and parry; Weapon Mastery removes
  a further 1% / 2% of dodge.
- **Armor**: base item armor × (1 + Toughness 2% per rank) + Agility × 2 + bonus
  armor and buffs; Battle Stance and Mace Specialization add armor penetration.
- **Rage costs** are stored in tenths in the DBC: Improved Heroic Strike,
  Improved Execute, Puncture, Focused Rage and Improved Thunder Clap subtract
  whole rage per rank; Intensify Rage shortens Bloodrage, Berserker Rage,
  Recklessness and Death Wish cooldowns by 11% per rank.
- **Execute**: base + extra rage × per-rage multiplier + 20% of attack power.
- **Shield Slam**: base + shield block value, soft-capped at level × 24.5 and
  hard-capped at level × 34.5 (doubled under Shield Block).
- **Weapon specializations**: physical damage +2% per rank with the matching
  weapon type (One-Handed: 1H axe/mace/sword/dagger/fist; Two-Handed: 2H
  axe/mace/sword and polearm).

## Tests

All tests need a build-12340 DBC directory:

```
tools/tests/run_local_warrior_talents_tests.sh    DBC_DIR   # 73 pinned ranks, column mutations, runtime effects
tools/tests/run_local_warrior_next_swing_tests.sh DBC_DIR   # Heroic Strike / Cleave queue
tools/tests/run_local_warrior_progression_tests.sh DBC_DIR
tools/tests/run_local_warrior_stats_tests.sh      DBC_DIR
```

## Next steps, in order of value

1. The Warrior **proc** family: Deep Wounds, Sword Specialization, Enrage,
   Taste for Blood, Sudden Death, Bloodsurge, Sword and Board, Second Wind,
   Trauma, Juggernaut. This is the largest block left, 19 talents.
2. Critical damage modifiers: Impale and Poleaxe Specialization. The ×2 melee
   critical multiplier is currently written out at several call sites.
3. Snapshotted shout amounts for Commanding Presence; stance-conditional talents
   (Improved Berserker Stance, Improved Defensive Stance).
4. Last Stand, Concussion Blow, Shockwave, Piercing Howl, Devastate, Titan's
   Grip, Bladestorm.
5. Tooltip expressions `${...}` and the `$a`, `$t`, `$h` and `$n` tokens.
