# Research notes — Issue #2 (BANKHiROMSlowROM black screen)

## Что проверено

Сравнил заголовки 4 BANK тестов (offsets 0x7FC0 LoROM-pos и 0xFFC0 HiROM-pos):

| Тест           | mapper @ HiROM-pos | Checksum |
|---|---|---|
| HiROMFastROM   | 0x21 | BAD (XOR=0x1000) |
| HiROMSlowROM   | 0x21 | BAD (XOR=0x1000) |
| LoROMFastROM   | 0x20 @ LoROM-pos | BAD |
| LoROMSlowROM   | 0x20 @ LoROM-pos | BAD |

Все 4 имеют BAD checksum → `verifyHeader` возвращает false для обеих позиций.
Дальше fallback в `detectMapMode` (snes_bus.cpp:117-125) смотрит на `mapByte`:
- HiROMFastROM/HiROMSlowROM: `hi=0x21` → `(0x21 & 0xEF)==0x21` → HiROM ✓ (оба!)

**Вывод:** детект НЕ ошибается. Оба HiROM-теста определяются как HiROM правильно.

## readHiROM выглядит корректно

bank=$00, addr=$FFFC (reset vector) → ROM offset 0xFFFC ✓

## Где искать дальше

Чёрный экран при rc=0 значит:
1. CPU отработал, эмулятор не упал
2. PPU не получил команд на рендеринг (или получил, но screen disabled)

Гипотезы (по убыванию вероятности):
1. **MEMSEL ($420D)**: код игнорирует регистр. ROM может ждать что после `STA $420D; A=1` система реально переключится на FastROM. Не переключение → код "зависает" в каком-то цикле опроса
2. **Reset vector в ROM-е** указывает на код, который для SlowROM-режима ожидает другие тайминги чем FastROM. Код может стучаться по $4212 (HVBJOY) и ждать конкретного значения которое в нашем эмуляторе не приходит вовремя
3. **Display disable ($2100 bit 7)** — код может не снимать display blank никогда

## Следующие шаги (для агента-coder когда будет время)

1. Запустить ROM с `--record-trace` и сравнить CPU PC во времени между HiROMFastROM (работает) и HiROMSlowROM (не работает)
2. Смотреть на PC после ~600 кадров: где код HiROMSlowROM застрял
3. Проверить SnesBus::write для $420D MEMSEL — добавить реакцию (даже если просто store, чтобы read возвращал записанное)

## Status

Оставляю **open** до отдельной итерации с детальным трейсом. Это не одношаговое исправление.

## Update — попытка фикса #1 (MEMSEL storage)

Добавлено storage для $420D в `snes_bus.cpp` (commit будет вместе с baseline-cleanup):
- Запись `$420D = X` теперь сохраняется в `memsel_`
- Чтение `$420D` возвращает `memsel_` (раньше open bus)

**Результат:** BANKHiROMSlowROM по-прежнему чёрный (758 byte PNG). MEMSEL не был причиной.

Гипотеза #1 из верхней секции ОТВЕРГНУТА. Идём к гипотезе #2 (reset vector / тайминги opcode'ов для SlowROM).

Следующий шаг — **запустить с `--record-trace` оба HiROM-теста и сравнить первые сотни PC**. Где траектории расходятся — там и баг.
