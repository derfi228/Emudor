#!/usr/bin/env python3
r"""
status_report.py — генерирует STATUS.md с обзором работы агентов.

Использование:
    python tools\status_report.py
    python tools\status_report.py --db agent_data\issues.db --out STATUS.md

Читает agent_data\issues.db и формирует читаемый Markdown-отчёт с:
- прогрессом по консолям,
- открытыми/in_progress/fixed/verified багами,
- историей test_runs,
- неотвеченными вопросами,
- активностью за последние 24 часа.

Перезаписывает выходной файл при каждом запуске.
"""
import argparse
import os
import sqlite3
import sys
from datetime import datetime
from pathlib import Path


# ─── Утилиты форматирования ─────────────────────────────────────────────────

SEVERITY_ORDER = {"critical": 1, "high": 2, "medium": 3, "low": 4}
STATUS_EMOJI = {"done": "✓", "in_progress": "●", "planned": "○"}


def fmt_ts(ts):
    """SQLite TIMESTAMP → '2026-05-31 14:32' (или пусто)."""
    if not ts:
        return ""
    s = str(ts).strip()
    # Поддерживаем 'YYYY-MM-DD HH:MM:SS' и 'YYYY-MM-DDTHH:MM:SS'
    s = s.replace("T", " ")
    if len(s) >= 16:
        return s[:16]
    return s


def truncate(text, limit=200):
    if not text:
        return ""
    t = str(text).strip().replace("\n", " ")
    if len(t) <= limit:
        return t
    return t[:limit].rstrip() + "..."


def short_commit(h):
    return (str(h)[:8]) if h else ""


def safe_str(v):
    return "" if v is None else str(v)


# ─── Генерация секций ───────────────────────────────────────────────────────

def section_header(db_path):
    size_kb = os.path.getsize(db_path) / 1024 if os.path.exists(db_path) else 0
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    return [
        "# Emudor — Agent Status Report",
        "",
        f"**Сгенерировано:** {now}  ",
        f"**База данных:** `{db_path}` ({size_kb:.1f} KB)",
        "",
    ]


def section_console_progress(conn):
    out = ["## Прогресс по консолям", ""]
    rows = list(conn.execute(
        "SELECT console, priority, status, tests_total, tests_passing, "
        "pass_rate, notes FROM console_progress ORDER BY priority"
    ))
    if not rows:
        out.append("(пусто)")
        out.append("")
        return out

    out.append("| Console | Priority | Status | Tests | Pass rate | Notes |")
    out.append("|---|---|---|---|---|---|")
    for r in rows:
        console, prio, status, total, passing, rate, notes = r
        emoji = STATUS_EMOJI.get(status, "?")
        tests = f"{passing or 0}/{total or 0}"
        rate_str = f"{(rate or 0) * 100:.1f}%"
        notes_str = truncate(notes, 60).replace("|", "\\|")
        out.append(
            f"| {console} | {prio} | {emoji} {status} | "
            f"{tests} | {rate_str} | {notes_str} |"
        )
    out.append("")
    return out


def _format_issue_block(row):
    """Формирует один блок issue для секций Open / In Progress."""
    iid, console, component, severity, status, title, description, \
        reproduction, screenshot_path, discovered_at = row
    return [
        "---",
        f"**#{iid}** `[{severity}]` **{console}** / {component or '-'}",
        f"**Title:** {safe_str(title)}",
        f"**Discovered:** {fmt_ts(discovered_at)}",
        f"**Description:** {truncate(description, 200)}",
        f"**Reproduction:** {truncate(reproduction, 200) or '-'}",
        f"**Screenshot:** "
        + (f"`{screenshot_path}`" if screenshot_path else "-"),
    ]


def section_open_issues(conn):
    out = ["## Открытые баги (status='open')", ""]
    rows = list(conn.execute(
        "SELECT id, console, component, severity, status, title, description, "
        "reproduction, screenshot_path, discovered_at "
        "FROM issues WHERE status='open'"
    ))
    # Сортируем по severity, потом по дате
    rows.sort(key=lambda r: (SEVERITY_ORDER.get(r[3], 99), r[9] or ""))
    if not rows:
        out.append("(пусто)")
        out.append("")
        return out
    for r in rows:
        out.extend(_format_issue_block(r))
    out.append("---")
    out.append("")
    return out


def section_in_progress(conn):
    out = ["## Баги в работе (status='in_progress')", ""]
    rows = list(conn.execute(
        "SELECT id, console, component, severity, status, title, description, "
        "reproduction, screenshot_path, discovered_at "
        "FROM issues WHERE status='in_progress'"
    ))
    rows.sort(key=lambda r: (SEVERITY_ORDER.get(r[3], 99), r[9] or ""))
    if not rows:
        out.append("(пусто)")
        out.append("")
        return out
    for r in rows:
        out.extend(_format_issue_block(r))
    out.append("---")
    out.append("")
    return out


def section_fixed(conn):
    out = ["## Исправленные, но не проверенные (status='fixed')", ""]
    rows = list(conn.execute(
        "SELECT id, title, fixed_in_commit FROM issues "
        "WHERE status='fixed' ORDER BY id DESC"
    ))
    if not rows:
        out.append("(пусто)")
        out.append("")
        return out
    for iid, title, commit in rows:
        out.append(f"- **#{iid}** {safe_str(title)} → commit `{short_commit(commit)}`")
    out.append("")
    return out


def section_verified(conn):
    out = ["## Verified (последние 10)", ""]
    rows = list(conn.execute(
        "SELECT id, title, verified_at FROM issues "
        "WHERE status='verified' ORDER BY verified_at DESC LIMIT 10"
    ))
    if not rows:
        out.append("(пусто)")
        out.append("")
        return out
    for iid, title, ts in rows:
        out.append(f"- **#{iid}** {safe_str(title)} — verified at {fmt_ts(ts)}")
    out.append("")
    return out


def section_test_runs(conn):
    out = ["## Прогоны тестов (последние 30)", ""]
    rows = list(conn.execute(
        "SELECT run_at, console, rom_name, result, screenshot_path "
        "FROM test_runs ORDER BY run_at DESC LIMIT 30"
    ))
    if not rows:
        out.append("(пусто)")
        out.append("")
        return out
    out.append("| Run at | Console | ROM | Result | Screenshot |")
    out.append("|---|---|---|---|---|")
    for run_at, console, rom, result, shot in rows:
        rom_short = truncate(rom, 40).replace("|", "\\|")
        shot_str = (f"`{shot}`" if shot else "-").replace("|", "\\|")
        out.append(
            f"| {fmt_ts(run_at)} | {safe_str(console)} | "
            f"{rom_short} | {safe_str(result)} | {shot_str} |"
        )
    out.append("")
    return out


def section_test_runs_summary(conn):
    out = ["## Сводка по test_runs", ""]
    consoles = [r[0] for r in conn.execute(
        "SELECT DISTINCT console FROM test_runs ORDER BY console"
    )]
    if not consoles:
        out.append("(пусто)")
        out.append("")
        return out
    out.append("| Console | Всего | Pass | Fail/Crash | Странные | Pass rate |")
    out.append("|---|---|---|---|---|---|")
    for c in consoles:
        total = conn.execute(
            "SELECT COUNT(*) FROM test_runs WHERE console=?", (c,)
        ).fetchone()[0]
        passed = conn.execute(
            "SELECT COUNT(*) FROM test_runs WHERE console=? AND result='pass'",
            (c,)
        ).fetchone()[0]
        failed = conn.execute(
            "SELECT COUNT(*) FROM test_runs WHERE console=? "
            "AND result IN ('fail', 'crash')", (c,)
        ).fetchone()[0]
        weird = conn.execute(
            "SELECT COUNT(*) FROM test_runs WHERE console=? "
            "AND result IN ('black_screen', 'frozen', 'glitch')", (c,)
        ).fetchone()[0]
        rate = (passed / total * 100) if total else 0
        out.append(
            f"| {c} | {total} | {passed} | {failed} | {weird} | {rate:.1f}% |"
        )
    out.append("")
    return out


def section_questions(conn):
    out = ["## Неотвеченные вопросы пользователю", ""]
    rows = list(conn.execute(
        "SELECT id, agent, asked_at, question, options "
        "FROM questions_for_user WHERE user_answer IS NULL "
        "ORDER BY asked_at DESC"
    ))
    if not rows:
        out.append("(пусто)")
        out.append("")
        return out
    for qid, agent, ts, question, options in rows:
        out.extend([
            "---",
            f"**#{qid}** от `{safe_str(agent)}`",
            f"**Asked at:** {fmt_ts(ts)}",
            f"**Вопрос:** {safe_str(question)}",
            f"**Варианты:** {safe_str(options) or '-'}",
        ])
    out.append("---")
    out.append("")
    return out


def section_activity(conn):
    out = ["## Активность за последние 24 часа", ""]
    new_issues = conn.execute(
        "SELECT COUNT(*) FROM issues "
        "WHERE discovered_at > datetime('now', '-1 day')"
    ).fetchone()[0]
    closed = conn.execute(
        "SELECT COUNT(*) FROM issues "
        "WHERE status='verified' AND verified_at > datetime('now', '-1 day')"
    ).fetchone()[0]
    runs = conn.execute(
        "SELECT COUNT(*) FROM test_runs "
        "WHERE run_at > datetime('now', '-1 day')"
    ).fetchone()[0]
    out.append(f"- Новых issues: **{new_issues}**")
    out.append(f"- Закрыто (verified): **{closed}**")
    out.append(f"- Прогонов тестов: **{runs}**")
    out.append("")
    return out


def section_footer():
    return [
        "---",
        "",
        "## Команды",
        "",
        "- Обновить отчёт: `python tools\\status_report.py`",
        "- Произвольный SELECT: `python tools\\db_query.py \"<SQL>\"`",
        "- INSERT/UPDATE: `python tools\\db_exec.py \"<SQL>\" <params...>`",
        "",
    ]


# ─── main ───────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--db", default="agent_data/issues.db",
                   help="Путь к SQLite БД (по умолчанию agent_data/issues.db)")
    p.add_argument("--out", default="STATUS.md",
                   help="Куда записать отчёт (по умолчанию STATUS.md в корне)")
    args = p.parse_args()

    if not os.path.exists(args.db):
        print(f"ERROR: DB not found: {args.db}", file=sys.stderr)
        return 1

    try:
        conn = sqlite3.connect(args.db)
    except sqlite3.Error as e:
        print(f"ERROR: cannot open DB: {e}", file=sys.stderr)
        return 1

    sections = []
    sections.extend(section_header(args.db))
    sections.extend(section_console_progress(conn))
    sections.extend(section_open_issues(conn))
    sections.extend(section_in_progress(conn))
    sections.extend(section_fixed(conn))
    sections.extend(section_verified(conn))
    sections.extend(section_test_runs(conn))
    sections.extend(section_test_runs_summary(conn))
    sections.extend(section_questions(conn))
    sections.extend(section_activity(conn))
    sections.extend(section_footer())

    content = "\n".join(sections)

    # Считаем сводку для финального print
    open_count = conn.execute(
        "SELECT COUNT(*) FROM issues WHERE status='open'"
    ).fetchone()[0]
    runs_count = conn.execute("SELECT COUNT(*) FROM test_runs").fetchone()[0]
    conn.close()

    out_path = Path(args.out)
    out_path.write_text(content, encoding="utf-8")
    size = out_path.stat().st_size

    print(
        f"Report saved to {out_path} ({size} bytes, "
        f"{open_count} open issues, {runs_count} test runs)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
