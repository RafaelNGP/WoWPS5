#!/usr/bin/env python3
"""Compile spell_target_position (AzerothCore, the pinned commit) for the
teleport spells players cast (SPELL_EFFECT_TELEPORT_UNITS, e.g. the mage
Teleport and Portal spells): one destination per spell, effect index 0.

    python3 -B tools/local_realm/import_spell_destinations.py <DBFilesClient dir> \
        --output assets/local_realm/spell_destinations.json [--sql spell_target_position.sql]
"""
from __future__ import annotations
import argparse, hashlib, json, sys, tempfile, urllib.request
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from import_azerothcore import sql_rows, PINNED_COMMIT
from import_consumables import dbc

URL = f"https://raw.githubusercontent.com/azerothcore/azerothcore-wotlk/{PINNED_COMMIT}/data/sql/base/db_world/spell_target_position.sql"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dbc_dir', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--sql', type=Path, help='a local copy of spell_target_position.sql (else downloaded)')
    args = ap.parse_args()
    data = args.sql.read_bytes() if args.sql else urllib.request.urlopen(URL, timeout=60).read()
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / 'spell_target_position.sql'; path.write_bytes(data)
        rows = list(sql_rows(path, 'spell_target_position'))
    spells, _ = dbc(args.dbc_dir / 'Spell.dbc')
    out = []
    for r in sorted(rows, key=lambda r: r['id']):
        if r['effectindex'] != 0 or r['id'] not in spells: continue
        u = spells[r['id']][0]
        if 5 not in (u[71], u[72], u[73]): continue  # only TELEPORT_UNITS spells
        out.append({'spellId': r['id'], 'mapId': r['mapid'], 'x': round(r['positionx'], 3), 'y': round(r['positiony'], 3),
                    'z': round(r['positionz'], 3), 'orientation': round(r['orientation'], 4)})
    doc = {'schemaVersion': 1, 'sourceCommit': PINNED_COMMIT, 'sourceSha256': hashlib.sha256(data).hexdigest(), 'destinations': out}
    args.output.write_text(json.dumps(doc, separators=(',', ':'), sort_keys=True) + '\n')
    print(json.dumps({'destinations': len(out)}))


if __name__ == '__main__':
    main()
