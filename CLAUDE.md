# Emudor
Мультиконсольный эмулятор на C++17. Текущие маперы: NROM (0), MMC1 (1), MMC3 (4), FME-7 (69).

## Окружение
MSYS2 MinGW64, GCC 16, CMake 4.3, Ninja 1.13.
Все команды запускать из MSYS2 MinGW64 shell или через:
`C:\msys64\usr\bin\bash.exe -lc "export PATH=/mingw64/bin:$PATH && cd /e/Claude_projects/Emu/NesEmu && <команда>"`

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

### Тесты
```bash
cmake --build build/debug --target nes_tests
./build/debug/nes_tests.exe
# или через ctest:
ctest --test-dir build/debug -V
```

### nestest
```bash
cmake --build build/debug --target nestest
./build/debug/nestest.exe roms/nestest.nes roms/nestest.log
```

### Переключение release/debug
```bash
cmake -B build/debug   -G Ninja -DCMAKE_BUILD_TYPE=Debug   && cmake --build build/debug
cmake -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/release
```

## Архитектура
- **CPU**: MOS 6502, таблица из 256 опкодов, cycle-accurate
- **PPU**: Ricoh 2C02, Loopy-регистры, 262 скэнлайна × 341 дот
- **MemoryBus**: маршрутизация $0000–$FFFF, OAM DMA, загрузка iNES
- **APU**: 2× pulse, triangle, noise, нелинейный микшер
- **App**: SDL2 + ImGui, управление состоянием, сохранения

## Правила кода
- `uint8_t` / `uint16_t` для всех регистров и адресов, никакого `int`
- Комментарии на русском, имена функций и переменных на английском
- Для каждого опкода CPU — юнит-тест в Google Test
- Не использовать глобальные переменные
- После каждого большого изменения: собрать и запустить тесты

## Порядок разработки
CPU → MemoryBus → PPU → APU → UI (App)
