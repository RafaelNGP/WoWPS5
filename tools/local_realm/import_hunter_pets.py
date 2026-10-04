#!/usr/bin/env python3
"""Compile the hunter pet companion: tameable beasts and the pet spells.

creature_template (AzerothCore, the pinned world_source_sql.tar.gz) marks a
tameable creature with CREATURE_TYPE_FLAG_TAMEABLE (type_flags & 1) on a beast
(type 1) with a pet family; CREATURE_TYPE_FLAG_EXOTIC_PET (0x10000) needs the
Beast Mastery talent and is left out. Only creatures the world catalog spawns
are listed. The player's own Spell.dbc/SpellIcon.dbc (3.3.5a build 12340)
give the pet spells' names, icons, cast times and cooldowns.

    python3 -B tools/local_realm/import_hunter_pets.py <DBFilesClient dir> \
        --output assets/local_realm/hunter_pets.json
"""
from __future__ import annotations
import argparse, json, struct, sys, tarfile, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from import_azerothcore import sql_rows, PINNED_COMMIT
from import_consumables import dbc

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
CATEGORY, RECOVERY, CAST_TIME_INDEX, ICON, NAME = 1, 29, 28, 133, 136
TAMEABLE, EXOTIC = 0x1, 0x10000
# Tame Beast, Call Pet, Dismiss Pet, Revive Pet: what a level 10 hunter
# learns to own a pet. All of them are learned at level 10.
PET_SPELLS = {1515: 10, 883: 10, 2641: 10, 982: 10}


def spawned_entries():
    data = (ROOT / 'assets/local_realm/catalog/spawns.pack').read_bytes()
    return {struct.unpack_from('<I', data, i + 4)[0] for i in range(0, len(data), 28)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dbc_dir', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--sources', type=Path, default=HERE / 'world_source_sql.tar.gz')
    args = ap.parse_args()
    spells, string = dbc(args.dbc_dir / 'Spell.dbc')
    icons, icon_string = dbc(args.dbc_dir / 'SpellIcon.dbc')
    cast_times = {k: v[1][1] for k, v in dbc(args.dbc_dir / 'SpellCastTimes.dbc')[0].items()}
    with tempfile.TemporaryDirectory() as tmp, tarfile.open(args.sources) as tar:
        tar.extract('creature_template.sql', tmp, filter='data')
        templates = list(sql_rows(Path(tmp) / 'creature_template.sql', 'creature_template'))
    spawned = spawned_entries()
    beasts = sorted(({'entry': t['entry'], 'family': t['family']} for t in templates
                     if t['type'] == 1 and t['type_flags'] & TAMEABLE and not t['type_flags'] & EXOTIC
                     and 0 < t['family'] < 256 and t['entry'] in spawned), key=lambda b: b['entry'])
    out_spells = []
    for spell_id, level in sorted(PET_SPELLS.items()):
        u, _ = spells[spell_id]
        icon = icon_string(icons[u[ICON]][0][1]) if u[ICON] in icons else ''
        out_spells.append({'id': spell_id, 'name': string(u[NAME])[:64], 'icon': icon[:128], 'level': level,
                           'castMs': max(0, cast_times.get(u[CAST_TIME_INDEX], 0)), 'cooldownMs': u[RECOVERY]})
    doc = {'schemaVersion': 1, 'sourceCommit': PINNED_COMMIT, 'clientBuild': 12340,
           'spells': out_spells, 'beasts': beasts}
    args.output.write_text(json.dumps(doc, separators=(',', ':'), sort_keys=True) + '\n')
    print(json.dumps({'spells': [s['name'] for s in out_spells], 'beasts': len(beasts)}))


if __name__ == '__main__':
    main()
