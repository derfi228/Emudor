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
    Звучат все игры из библиотеки: Super Mario World, Zelda, Super Mario Kart, Star Fox,
    EarthBound, Super Street Fighter II (замер уровня выхода). Эха (echo/FIR) нет.
  - **DSP-1** — HLE на уровне команд (Mario Kart: трассы, перспектива Mode 7).
  - **SuperFX/GSU** — полное ядро: конвейер с delay slot, кэш команд, буферы
    ПЗУ/ОЗУ, плоттер PLOT/RPIX, блокировка шины, IRQ. Star Fox играется.
  - **Save states** полные: CPU, PPU (VRAM/CGRAM/OAM), звук (SPC700+DSP), шина с DMA,
    SuperFX, DSP-1. Загрузка продолжает эмуляцию байт-в-байт; чужой/старый файл отвергается.
  - Не сделано: эхо DSP.
- **Game Boy / Game Boy Color** — работает. CPU SM83 (все 512 опкодов, точность до
  M-цикла), PPU (фон, окно, спрайты, STAT, CGB-палитры и банки), APU (4 канала), таймер,
  джойпад, OAM DMA, HDMA, двойная скорость CGB, MBC1/2/3 (с часами)/5, батарейка
  (формат VBA/BGB), save states. Режим (DMG/CGB) — по заголовку картриджа.
- **GBA, N64, PS1** — запланированы, не начаты.

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
./build/debug/emudor.exe --rom <path> --console NES|SNES|GB --frames N \
  --screenshot out.png --screenshot-every 60 --headless --record-trace trace.log
```
Доступные флаги: `--rom`, `--console`, `--frames`, `--screenshot`,
`--screenshot-every`, `--screenshot-from`, `--headless`, `--record-trace`, `--input-script`,
`--save-state <path> --save-state-at <N>`, `--load-state <path>`, `--hash-from <K>`, `--help`.
Проверка save state: хэш прогона «N+M кадров» (`--hash-from N+1`) должен совпасть с хэшем
«загрузка на кадре N + M кадров» (`--hash-from 1`).

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
- **SnesPPU**: режимы 0-7, Mode 7 (со спрайтами, EXTBG, прямым цветом), overscan 239 строк
  ($2133 бит 2: VBlank на строке 240), цветовое окно CGWSEL (`src/snes/snes_ppu.cpp`)
- **SnesAPU**: SPC700 (полный набор опкодов) + настоящий DSP (BRR ADPCM, 8 голосов).
  Синхронизация CPU↔SPC700 — co-scheduler по общему мастер-такту (`SnesConsole::runFrame`):
  PPU тикает по доту, CPU раз в 2 дота, а SPC700 «дозревает» по `spcNextTick_`. Порты
  $2140–$2143 — просто общие массивы, без burst/flush (`src/snes/snes_apu.cpp`)
- **SnesDSP1**: математический сопроцессор Mario Kart, HLE (`src/snes/snes_dsp1.cpp`)
- **SuperFX/GSU**: ядро GSU + плоттер, общая с CPU game-pak RAM (`src/snes/superfx.cpp`).
  Диагностика: `EMUDOR_GSU_TRACE=1` — состояние чипа и CPU раз в кадр

### Game Boy / Game Boy Color (`src/gb/`)
- **GbCpu**: SM83, каждое обращение к памяти — M-цикл через шину (`gb_cpu.cpp`)
- **GbBus**: карта памяти, таймер (спад бита делителя), джойпад, OAM DMA, HDMA,
  банки WRAM/двойная скорость CGB (`gb_bus.cpp`)
- **GbPpu**: строка рисуется в конце режима 3; прерывание STAT по фронту общей линии (`gb_ppu.cpp`)
- **GbApu**: 2 меандра, волна, шум, секвенсор 512 Гц, моно 44100 Гц (`gb_apu.cpp`)
- **GbCart**: MBC1/2/3/5, часы MBC3, батарейка (`gb_cart.cpp`)
- **GbConsole**: IConsole; кадр до VBlank (70224 точки). Диагностика: `EMUDOR_GB_TRACE=1`,
  `EMUDOR_GB_SERIAL=1` (вывод последовательного порта в stdout — так печатают тесты Blargg)
- **Тестовые ROM** (в git не входят, кладутся в `roms/gb/tests`): Blargg из
  github.com/retrio/gb-test-roms — `cpu_instrs.gb`, `instr_timing.gb`, `mem_timing.gb`,
  `halt_bug.gb`, `dmg_sound.gb`; Matt Currie — `dmg-acid2.gb`, `cgb-acid2.gbc` (релизы на
  GitHub). Все проходят; `tests/gb_testroms_test.cpp` гоняет их, если файлы на месте.

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
