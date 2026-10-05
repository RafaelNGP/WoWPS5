#!/usr/bin/env python3
"""Compile item durability for the standalone realm.

item_template.MaxDurability (AzerothCore, the pinned world_source_sql.tar.gz)
for every weapon and armor piece that has one, and the repair cost per point
of lost durability the reference charges (Player::DurabilityRepair):
DurabilityCosts.dbc[ItemLevel].multiplier[weapon subclass | armor subclass + 21]
times DurabilityQuality.dbc[(Quality + 1) * 2], from the player's own client.

    python3 -B tools/local_realm/import_durability.py <DBFilesClient dir> \
        --output assets/local_realm/durability.json
"""
from __future__ import annotations
import argparse, json, struct, sys, tarfile, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from import_azerothcore import sql_rows, PINNED_COMMIT

HERE = Path(__file__).resolve().parent


def rows(path, fmt):
    b = path.read_bytes(); n, f, size, _ = struct.unpack_from('<4I', b, 4)
    return {struct.unpack_from('<I', b, 20 + i * size)[0]: struct.unpack_from(fmt(f), b, 20 + i * size) for i in range(n)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dbc_dir', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--sources', type=Path, default=HERE / 'world_source_sql.tar.gz')
    args = ap.parse_args()
    costs = rows(args.dbc_dir / 'DurabilityCosts.dbc', lambda f: f'<{f}I')
    quality = rows(args.dbc_dir / 'DurabilityQuality.dbc', lambda f: '<If')
    with tempfile.TemporaryDirectory() as tmp, tarfile.open(args.sources) as tar:
        tar.extract('item_template.sql', tmp, filter='data')
        items = list(sql_rows(Path(tmp) / 'item_template.sql', 'item_template'))
    out = []
    for it in sorted(items, key=lambda r: r['entry']):
        if it['class'] not in (2, 4) or it['maxdurability'] <= 0: continue
        row = costs.get(it['itemlevel']); mod = quality.get((it['quality'] + 1) * 2)
        column = it['subclass'] if it['class'] == 2 else it['subclass'] + 21
        per_point = row[1 + column] * mod[1] if row and mod and 0 <= column < 29 else 0.0
        out.append([it['entry'], min(it['maxdurability'], 1000), int(round(per_point * 1000))])
    doc = {'schemaVersion': 1, 'sourceCommit': PINNED_COMMIT, 'items': out}
    args.output.write_text(json.dumps(doc, separators=(',', ':')) + '\n')
    print(json.dumps({'items': len(out)}))


if __name__ == '__main__':
    main()
