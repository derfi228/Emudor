---
description: Переход к следующей консоли из плана. ШАГ 6 orchestrator'а — обязательно через диалог с пользователем.
---

Запусти `orchestrator` для выполнения **ШАГА 6** (переход к новой консоли).

# Что делает эта команда

1. Определяет текущую `in_progress` и следующую `planned` консоль из `console_progress` (порядок по `priority`).
2. **Создаёт `question_for_user`** с приглашением обсудить архитектуру новой консоли.
3. **Останавливается и ждёт ответ пользователя в чате** (НЕ в options — реальное обсуждение).
4. Только после явного ТЗ от пользователя — переключает `console_progress` и (в роли coder) создаёт базовый скелет в `src/<console>/`.

# Что ОБЯЗАТЕЛЬНО спросить у пользователя

В чате после команды задай (создай 3 отдельных вопроса через `python tools\db_exec.py`):

1. **Архитектура CPU.** Для новой консоли (например, Game Boy → SM83): cycle-accurate (каждый T-цикл) vs instruction-stepped (каждый опкод). Cycle-accurate точнее, ступенчатый быстрее писать.

2. **Какие первые маперы / спецчипы.** Для GB: только MBC1 → играем большинство кассет; +MBC3 (RTC) → Pokemon, Zelda; +MBC5 → совместимость с GBC. Для GBC надстройка над GB, маперы те же. Для GBA — отдельная история (ARM7TDMI).

3. **Известные особенности железа которые блокируют простые ROM-ы.** Например, для GB критичен LCD STAT IRQ и DMA-копирование OAM. Без них половина игр не запустится. Спроси какие приоритеты.

# После того как пользователь ответил

(Только после того как пользователь написал в чате ответы — не после option-клика, а реальный текст ТЗ.)

1. Обнови БД:
   ```powershell
   python tools\db_exec.py "UPDATE console_progress SET status='done', completed_at=CURRENT_TIMESTAMP WHERE console=?" <OLD>
   python tools\db_exec.py "UPDATE console_progress SET status='in_progress', started_at=CURRENT_TIMESTAMP WHERE console=?" <NEW>
   ```

2. **Играй роль `coder`** (читай `coder.md`):
   - Создай feature-ветку: `git checkout -b feat/init-<console>-skeleton`
   - Сгенерируй структуру:
     ```
     src/<console>/
     ├── cpu.cpp/.h
     ├── ppu.cpp/.h
     ├── apu.cpp/.h
     ├── bus.cpp/.h
     src/console/<console>_console.cpp/.h   (реализация IConsole)
     ```
   - Обнови `src/console/console_detect.h` (новое расширение → ConsoleType)
   - Обнови `CMakeLists.txt` (добавь файлы в `nes_core`)
   - Обнови `src/cli.cpp` `makeConsole()` чтобы понимал новый ConsoleType
   - Собери `cmake --build build/debug --parallel` — должно скомпилироваться
   - Прогони `.\build\debug\nes_tests.exe` — все 46 тестов должны остаться зелёными
   - Коммит: `git commit -m "Init <console> skeleton based on user spec"`

3. Создай инициализационный issue:
   ```powershell
   python tools\db_exec.py "INSERT INTO issues (console, component, severity, status, title, description) VALUES (?, ?, ?, ?, ?, ?)" <NEW> all critical open "Реализовать минимально работающий стек <NEW>" "<ТЗ пользователя дословно>"
   ```

4. **НЕ мержь в main.** Сообщи пользователю: «Ветка `feat/init-<console>-skeleton` готова. Проверь и сделай merge сам.»

# Правила безопасности (те же что в orchestrator.md)

- ❌ Не переключай `console_progress` до подтверждения ТЗ от пользователя
- ❌ Не делай `git push` и `git merge` к main
- ❌ Не создавай скелет «по своему усмотрению» без ТЗ
