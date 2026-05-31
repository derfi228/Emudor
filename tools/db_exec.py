#!/usr/bin/env python3
r"""
db_exec.py — выполнить INSERT/UPDATE/DELETE с параметрами (без риска SQL-инъекций).

Использование:
    # INSERT issue
    python tools\db_exec.py "INSERT INTO issues (console, component, severity, status, title, description) VALUES (?, ?, ?, ?, ?, ?)" SNES CPU high open "Краткое название" "Подробное описание"

    # UPDATE статуса
    python tools\db_exec.py "UPDATE issues SET status=? WHERE id=?" in_progress 42

    # INSERT test_run
    python tools\db_exec.py "INSERT INTO test_runs (console, rom_name, result, screenshot_path) VALUES (?, ?, ?, ?)" SNES "CPUTest.sfc" pass "agent_data/screenshots/foo.png"

    # INSERT question_for_user (options как JSON-строка)
    python tools\db_exec.py "INSERT INTO questions_for_user (agent, question, options) VALUES (?, ?, ?)" coder "Какой подход выбрать?" "[\"А\", \"Б\"]"

Печатает: вставленный rowid (для INSERT) или количество затронутых строк (для UPDATE/DELETE).
"""
import argparse
import sqlite3
import sys


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--db", default="agent_data/issues.db",
                   help="Путь к SQLite базе (по умолчанию agent_data/issues.db)")
    p.add_argument("sql", help="SQL с ?-placeholder'ами")
    p.add_argument("params", nargs="*", help="Значения для placeholder'ов")
    args = p.parse_args()

    try:
        conn = sqlite3.connect(args.db)
        cur = conn.execute(args.sql, args.params)
        conn.commit()
        if cur.lastrowid and args.sql.strip().lower().startswith("insert"):
            print(f"INSERTED id={cur.lastrowid}")
        else:
            print(f"AFFECTED rows={cur.rowcount}")
        conn.close()
        return 0
    except sqlite3.Error as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
