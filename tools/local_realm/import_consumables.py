#!/usr/bin/env python3
"""Compile on-use consumables (food, drink, potions, healthstones, bandages).

item_template (AzerothCore, the pinned world_source_sql.tar.gz) names the spell
an item casts on use and its item/category cooldowns. The player's own
Spell.dbc/SpellDuration.dbc (3.3.5a build 12340) give what that spell does.
Only effects the local realm executes are kept: instant heal (effect 10),
instant mana (effect 30, power 0), health/mana regeneration auras (84; 85 with
its periodic-dummy amount 226 for drinks), periodic heal (aura 8) and periodic
mana (aura 24), followed through one trigger-spell level (Refreshment).
Buff food, elixirs, flasks, scrolls and every other aura remain unsupported and
are counted in the report rather than approximated.

    python3 -B tools/local_realm/import_consumables.py <DBFilesClient dir> \
        --output assets/local_realm/consumables.json
"""
from __future__ import annotations
import argparse, collections, json, struct, sys, tarfile, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from import_azerothcore import sql_rows, PINNED_COMMIT

HERE = Path(__file__).resolve().parent
# Spell.dbc 3.3.5a columns.
CATEGORY, ATTRIBUTES, RECOVERY, CATEGORY_RECOVERY = 1, 4, 29, 30
AURA_INTERRUPT, CHANNEL_INTERRUPT, DURATION_INDEX = 32, 33, 40
EFFECT, DIE_SIDES, BASE_POINTS, AURA, AMPLITUDE, MISC, TRIGGER = 71, 74, 80, 95, 98, 110, 116
NAME, ICON = 136, 133
ATTR0_CANT_USE_IN_COMBAT = 0x10000000
INTERRUPT_DAMAGE, INTERRUPT_MOVE, INTERRUPT_TURN, INTERRUPT_NOT_SEATED = 0x2, 0x8, 0x10, 0x40000
RECENTLY_BANDAGED_MS = 60000  # Spell 11196, applied by every First Aid bandage.


def dbc(path):
    b = path.read_bytes()
    if b[:4] != b'WDBC': raise ValueError(f'{path}: not a DBC')
    n, fields, size, strings = struct.unpack_from('<4I', b, 4)
    rows = {}
    for i in range(n):
        o = 20 + i * size
        rows[struct.unpack_from('<I', b, o)[0]] = (struct.unpack_from(f'<{fields}I', b, o), struct.unpack_from(f'<{fields}i', b, o))
    text = b[20 + n * size:]
    return rows, lambda off: text[off:text.index(b'\0', off)].decode('utf-8', 'replace')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dbc_dir', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--sources', type=Path, default=HERE / 'world_source_sql.tar.gz')
    args = ap.parse_args()
    spells, string = dbc(args.dbc_dir / 'Spell.dbc')
    durations = {k: v[1][1] for k, v in dbc(args.dbc_dir / 'SpellDuration.dbc')[0].items()}
    icons, icon_string = dbc(args.dbc_dir / 'SpellIcon.dbc')
    with tempfile.TemporaryDirectory() as tmp, tarfile.open(args.sources) as tar:
        tar.extract('item_template.sql', tmp, filter='data')
        items = list(sql_rows(Path(tmp) / 'item_template.sql', 'item_template'))

    def effects(spell_id, depth=0):
        """Yield (unsigned row, signed row, effect index) for a spell and its triggers."""
        if spell_id not in spells: return
        u, s = spells[spell_id]
        for e in range(3):
            if u[EFFECT + e] == 64 and depth == 0 and u[TRIGGER + e]:
                yield from effects(u[TRIGGER + e], 1)
            elif u[EFFECT + e]:
                yield u, s, e

    out, report, buff_spells = [], collections.Counter(), {}
    for item in items:
        if item['class'] != 0: continue
        report['consumables'] += 1
        entry = {'itemId': item['entry'], 'instantHealth': 0, 'instantMana': 0, 'regenHealth': 0, 'regenMana': 0,
                 'durationMs': 0, 'cooldownMs': 0, 'category': 0, 'categoryCooldownMs': 0,
                 'requiredLevel': max(0, item['requiredlevel']), 'noCombat': False,
                 'cancelOnMove': False, 'cancelOnDamage': False, 'spellId': 0}
        for slot in range(1, 6):
            spell_id = item[f'spellid_{slot}']
            if not spell_id or item[f'spelltrigger_{slot}'] != 0 or spell_id not in spells: continue
            u, s = spells[spell_id]
            cooldown, category, category_cd = item[f'spellcooldown_{slot}'], item[f'spellcategory_{slot}'], item[f'spellcategorycooldown_{slot}']
            if cooldown < 0: cooldown = u[RECOVERY]
            if category_cd < 0 or not category: category, category_cd = u[CATEGORY], u[CATEGORY_RECOVERY]
            entry['cooldownMs'] = max(entry['cooldownMs'], cooldown)
            if category and category_cd > entry['categoryCooldownMs']: entry['category'], entry['categoryCooldownMs'] = category, category_cd
            per5 = collections.Counter()
            for eu, es, e in effects(spell_id):
                effect, aura, misc = eu[EFFECT + e], eu[AURA + e], es[MISC + e]
                low, high = es[BASE_POINTS + e] + 1, es[BASE_POINTS + e] + max(1, es[DIE_SIDES + e])
                amount = max(0, (low + high) // 2)
                duration = max(0, durations.get(eu[DURATION_INDEX], 0))
                entry['noCombat'] |= bool(eu[ATTRIBUTES] & ATTR0_CANT_USE_IN_COMBAT)
                interrupts = eu[AURA_INTERRUPT] | eu[CHANNEL_INTERRUPT]
                if effect == 10: entry['instantHealth'] += amount
                elif effect == 30 and misc == 0: entry['instantMana'] += amount
                elif effect in (6, 27, 35) and duration:
                    kind = None
                    if aura == 84: kind, total = 'regenHealth', amount * duration // 5000
                    elif aura in (85, 226) and misc == 0: per5[(eu[0], aura)] = amount; continue
                    elif aura == 8 and eu[AMPLITUDE + e]: kind, total = 'regenHealth', amount * (duration // eu[AMPLITUDE + e])
                    elif aura == 24 and misc == 0 and eu[AMPLITUDE + e]: kind, total = 'regenMana', amount * (duration // eu[AMPLITUDE + e])
                    if kind:
                        entry[kind] += total; entry['durationMs'] = max(entry['durationMs'], duration)
                        entry['spellId'] = entry['spellId'] or spell_id
                        entry['cancelOnMove'] |= bool(interrupts & (INTERRUPT_MOVE | INTERRUPT_TURN | INTERRUPT_NOT_SEATED))
                        entry['cancelOnDamage'] |= bool(interrupts & INTERRUPT_DAMAGE)
            # Drinks carry their mana-per-5 in a periodic dummy (226) beside an
            # empty aura 85; take the larger of the two per spell, not their sum.
            for spell in {k[0] for k in per5}:
                amount = max(per5[(spell, 85)], per5[(spell, 226)])
                u2 = spells[spell][0]; duration = max(0, durations.get(u2[DURATION_INDEX], 0))
                if amount and duration:
                    entry['regenMana'] += amount * duration // 5000; entry['durationMs'] = max(entry['durationMs'], duration)
                    entry['spellId'] = entry['spellId'] or spell_id
                    interrupts = u2[AURA_INTERRUPT] | u2[CHANNEL_INTERRUPT]
                    entry['cancelOnMove'] |= bool(interrupts & (INTERRUPT_MOVE | INTERRUPT_TURN | INTERRUPT_NOT_SEATED))
                    entry['cancelOnDamage'] |= bool(interrupts & INTERRUPT_DAMAGE)
        if item['subclass'] == 7 and entry['regenHealth']:
            entry['category'], entry['categoryCooldownMs'] = 11196, max(entry['categoryCooldownMs'], RECENTLY_BANDAGED_MS)
        if not any(entry[k] for k in ('instantHealth', 'instantMana', 'regenHealth', 'regenMana')):
            report['unsupported'] += 1; continue
        if not (entry['regenHealth'] or entry['regenMana']): entry['durationMs'] = 0; entry['spellId'] = 0
        if entry['spellId']:
            # The buff the player sees while eating: the on-use spell's own name and icon.
            u = spells[entry['spellId']][0]
            icon = icon_string(icons[u[ICON]][0][1]) if u[ICON] in icons else ''
            buff_spells[entry['spellId']] = {'id': entry['spellId'], 'name': string(u[NAME])[:64], 'icon': icon[:128]}
        for k in ('instantHealth', 'instantMana', 'regenHealth', 'regenMana'): entry[k] = min(entry[k], 1000000)
        entry['cooldownMs'] = min(entry['cooldownMs'], 3600000); entry['categoryCooldownMs'] = min(entry['categoryCooldownMs'], 3600000)
        report['supported'] += 1
        out.append(entry)
    out.sort(key=lambda e: e['itemId'])
    doc = {'schemaVersion': 1, 'sourceCommit': PINNED_COMMIT, 'clientBuild': 12340,
           'report': dict(sorted(report.items())), 'items': out,
           'spells': [buff_spells[k] for k in sorted(buff_spells)]}
    args.output.write_text(json.dumps(doc, separators=(',', ':'), sort_keys=True) + '\n')
    print(json.dumps(doc['report']))


if __name__ == '__main__':
    main()
