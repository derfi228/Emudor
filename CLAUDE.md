# Emudor
Мультиконсольный эмулятор на C++17. Текущие маперы NES: NROM (0), MMC1 (1), MMC3 (4), FME-7 (69).

## Окружение
MSYS2 MinGW64, GCC 16, CMake 4.3, Ninja 1.13.
Все команды запускать из MSYS2 MinGW64 shell или через:
`C:\msys64\usr\bin\bash.exe -lc "export PATH=/mingw64/bin:$PATH && cd /e/Claude_projects/Emu/Emudor && <команда>"`

## Текущее состояние
- **NES** — готов. CPU 6502 cycle-accurate, PPU 2C02, APU (2× pulse + triangle + noise),
  4 маппера (NROM/MMC1/MMC3/FME-7), Battery SRAM, save states. 46 юнит-тестов проходят.
- **SNES** — в активной работе, играбелен. CPU 65816, PPU (режимы 0-7, Mode 7,
  Mosaic, приоритеты спрайт↔фон, OPHCT/OPVCT), SnesBus (HiROM/LoROM, DMA, HDMA,
  auto-joypad, multiply/divide).
  - **Звук работает**: SPC700 (полный набор опкодов) + DSP (настоящий BRR ADPCM,
    8 голосов, огибающие, микс). Синхронизация — co-scheduler по мастер-такту.
    Звучат: Super Mario World, Zelda, Super Mario Kart. Молчат: EarthBound (KON не
    выставляется), Super Street Fighter II (драйвер грузится, но тишина). Эха (echo/FIR) нет.
  - **DSP-1** — минимальная заглушка (Mario Kart грузится, меню/музыка ок; трассы нет).
  - **SuperFX/GSU** — каркас есть, часть GSU-тест-ROM падает/чёрный экран.
  - Не сделано: цветовая математика (color math/прозрачность) частично, эхо DSP,
    полная математика DSP-1 и SuperFX-рендера.
- **GB, GBC, GBA, N64, PS1** — запланированы, не начаты.

> Подробный живой статус (что играет, какие баги, скриншоты) — в `STATUS.md`.

## Команды

### Первая настройка
```bash
export PATH=/mingw64/bin:$PATH
cd /e/Claude_projects/Emu/Emudor
cmake -B build/debug   -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
```

### Сборка
```bash
cmake --build build/debug   --parallel   # debug
cmake --build build/release --parallel   # release
```

### Запуск эмулятора
```bash
./build/debug/emudor.exe     # debug
./build/release/emudor.exe   # release
```

### Headless-режим (для агентов / автотестов)
```bash
./build/debug/emudor.exe --rom <path> --console NES|SNES --frames N \
  --screenshot out.png --screenshot-every 60 --headless --record-trace trace.log
```
Доступные флаги: `--rom`, `--console`, `--frames`, `--screenshot`,
`--screenshot-every`, `--headless`, `--record-trace`, `--help`.

### Тесты
```bash
cmake --build build/debug --target nes_tests
./build/debug/nes_tests.exe
# или через ctest:
ctest --test-dir build/debug -V
```

### nestest (NES CPU regression)
```bash
cmake --build build/debug --target nestest
./build/debug/nestest.exe roms/nes/nestest.nes roms/nes/nestest.log
```

### Переключение release/debug
```bash
cmake -B build/debug   -G Ninja -DCMAKE_BUILD_TYPE=Debug   && cmake --build build/debug
cmake -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/release
```

## Архитектура
### NES
- **CPU**: MOS 6502, таблица из 256 опкодов, cycle-accurate (`src/cpu/`)
- **PPU**: Ricoh 2C02, Loopy-регистры, 262 скэнлайна × 341 дот (`src/ppu/`)
- **MemoryBus**: маршрутизация $0000–$FFFF, OAM DMA, загрузка iNES (`src/memory/`)
- **APU**: 2× pulse, triangle, noise, нелинейный микшер (`src/apu/`)
- **Mappers**: 0/1/4/69 (`src/mapper/`)

### SNES
- **CPU 65816**: 8/16-бит режимы, banking (`src/snes/cpu65816.cpp`)
- **SnesBus**: HiROM/LoROM auto-detect, DMA, HDMA, auto-joypad, multiply/divide (`src/snes/snes_bus.cpp`)
- **SnesPPU**: режимы 0-7, Mode 7 affine, sprite caching (`src/snes/snes_ppu.cpp`)
- **SnesAPU**: SPC700 (полный набор опкодов) + настоящий DSP (BRR ADPCM, 8 голосов).
  Синхронизация CPU↔SPC700 — co-scheduler по общему мастер-такту (`SnesConsole::runFrame`):
  PPU тикает по доту, CPU раз в 2 дота, а SPC700 «дозревает» по `spcNextTick_`. Порты
  $2140–$2143 — просто общие массивы, без burst/flush (`src/snes/snes_apu.cpp`)
- **SnesDSP1**: математический сопроцессор Mario Kart — заглушка (`src/snes/snes_dsp1.cpp`)
- **SuperFX/GSU**: каркас (`src/snes/superfx.cpp`)

### Общее
- **IConsole** (`src/console/iconsole.h`) — абстрактный интерфейс для всех консолей
- **App** (`src/app.cpp`): SDL2 + ImGui, ROM library, обложки, бинды, save-слоты
- **CLI** (`src/cli.cpp`): headless-runner для авто-тестов

## Правила кода
- `uint8_t` / `uint16_t` для всех регистров и адресов, никакого `int`
- Комментарии на русском, имена функций и переменных на английском
- Для каждого опкода CPU — юнит-тест в Google Test
- Не использовать глобальные переменные
- После каждого большого изменения: собрать и запустить тесты
- Не трогать `third_party/imgui` и `third_party/tinyfiledialogs` (вендорные копии)

## Порядок разработки
CPU → MemoryBus → PPU → APU → UI (App)
