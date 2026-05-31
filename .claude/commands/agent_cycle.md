---
description: Один полный цикл работы агентов над текущей "in_progress" консолью (шаги 1-5 orchestrator'а).
---

Запусти `orchestrator` для выполнения **одного полного цикла** (шаги 1–5) над текущей "in_progress" консолью.

# Prerequisites — проверь ДО запуска цикла

Выполни в PowerShell и выведи результаты в чат:

```powershell
# 1. База данных
if (-not (Test-Path "agent_data\issues.db")) {
    Write-Host "❌ agent_data\issues.db не найдена. Запусти sqlite-инициализацию из agent_data\init_db.sql + migration_progress.sql"
    exit 1
}

# 2. Бинарник эмулятора
if (-not (Test-Path "build\debug\emudor.exe")) {
    Write-Host "❌ build\debug\emudor.exe не собран. Выполни: cmake --build build/debug --parallel"
    exit 1
}
$exeAge = (Get-Date) - (Get-Item "build\debug\emudor.exe").LastWriteTime
if ($exeAge.TotalHours -gt 24) {
    Write-Host "⚠️  build\debug\emudor.exe старше 24 часов. Рекомендую: cmake --build build/debug --parallel"
}

# 3. Утилиты для БД
if (-not (Test-Path "tools\db_query.py") -or -not (Test-Path "tools\db_exec.py")) {
    Write-Host "❌ tools\db_query.py или tools\db_exec.py отсутствуют"
    exit 1
}

# 4. Текущая консоль
python tools\db_query.py "SELECT console FROM console_progress WHERE status='in_progress'"
```

Если текущая консоль найдена (например, SNES) — проверь наличие ROM-ов для неё:

```powershell
# 5. test_roms для текущей консоли
$testRoms = Get-ChildItem -Path "test_roms\snes" -Recurse -Include "*.sfc","*.smc" -ErrorAction SilentlyContinue
if (-not $testRoms) {
    Write-Host "⚠️  test_roms\snes пуст. Шаг 1 (TEST_ROMS) будет пропущен."
} else {
    Write-Host "✓ test_roms: $($testRoms.Count) файлов"
}

# 6. roms для текущей консоли (реальные игры)
$games = Get-ChildItem -Path "roms\snes" -Filter "*.sfc" -ErrorAction SilentlyContinue
if (-not $games) {
    Write-Host "⚠️  roms\snes пуст. Шаг 2 (GAMES) будет пропущен."
} else {
    Write-Host "✓ roms (игры): $($games.Count) файлов"
}
```

**Если что-то критичное отсутствует (БД или exe) — остановись, выведи понятное сообщение, дальше не иди.**

# Запуск

Прочитай `.claude\agents\orchestrator.md` и выполни его алгоритм (шаги 1–5) для текущей "in_progress" консоли.

## Лимиты на один прогон

Если пользователь не задал явно — используй разумные лимиты:
- Шаг 1: **первые 30 файлов** из `test_roms\<console>\` по алфавиту (полный прогон 2050 файлов SNES слишком долгий для одного цикла)
- Шаг 2: **все** файлы из `roms\<console>\` (там обычно мало)
- Шаги 3-4: **до 3 issues** за итерацию (топ по severity)
- Максимум **10 итераций** или **500 запусков emudor.exe** — что наступит раньше
- Если потрачено >200k токенов — пауза + вопрос пользователю

## Что не делаешь

- Не запускаешь `/next_console` (это отдельная команда)
- Не мержишь в main, не делаешь push
- Не переключаешь console_progress

## После завершения

- `python tools\status_report.py` — обнови STATUS.md
- Выведи в чат финальный отчёт от orchestrator'а
