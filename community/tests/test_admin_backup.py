"""admin.py backup: a consistent snapshot of the live database, pruned to --keep."""
import os
import sqlite3

import admin


def test_backup_copies_and_prunes(h, tmp_path):
    h.link(bytes([0x71]) * 32, name="Skeleton")
    out_dir = str(tmp_path / "bk")
    paths = [admin.backup_db(h.cfg.db_path, out_dir, keep=2, now=1800000000 + i) for i in range(3)]
    left = sorted(os.listdir(out_dir))
    assert left == sorted(os.path.basename(p) for p in paths[1:])
    with sqlite3.connect(paths[-1]) as db:
        assert db.execute("SELECT name FROM accounts").fetchone()[0] == "Skeleton"
    assert os.stat(paths[-1]).st_mode & 0o777 == 0o600
