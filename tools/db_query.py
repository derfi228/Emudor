#!/usr/bin/env python3
r"""
db_query.py — выполнить SQL SELECT и вывести результаты построчно.

Использование:
    python tools\db_query.py "SELECT id, severity, title FROM issues WHERE status='open'"
    python tools\db_query.py --db agent_data\issues.db "SELECT * FROM console_progress"

Вывод: одна строка на запись, поля разделены | (pipe).
"""
import argparse
import sqlite3
import sys


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--db", default="agent_data/issues.db",
                   help="Путь к SQLite базе (по умолчанию agent_data/issues.db)")
    p.add_argument("sql", help="SQL-запрос (любой SELECT)")
    args = p.parse_args()

    try:
        conn = sqlite3.connect(args.db)
        cur = conn.execute(args.sql)
        cols = [c[0] for c in cur.description] if cur.description else []
        if cols:
            print(" | ".join(cols))
            print("-" * 60)
        for row in cur:
            print(" | ".join("" if v is None else str(v) for v in row))
        conn.close()
        return 0
    except sqlite3.Error as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
