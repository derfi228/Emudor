# How to Run — Emudor

## Зависимости

- **MSYS2** установлен в `C:\msys64`
- MinGW64 пакеты (устанавливаются автоматически):

```bash
C:\msys64\usr\bin\bash.exe -lc "pacman -Sy --noconfirm \
  mingw-w64-x86_64-SDL2 \
  mingw-w64-x86_64-SDL2_image \
  mingw-w64-x86_64-nlohmann-json \
  mingw-w64-x86_64-gtest"
```

## Сборка

Все команды выполняются из MSYS2 MinGW64 shell, либо через:
```
C:\msys64\usr\bin\bash.exe -lc "export PATH=/mingw64/bin:$PATH && cd /e/Claude_projects/Emu/NesEmu && <команда>"
```

### Первая настройка (один раз)
```bash
export PATH=/mingw64/bin:$PATH
cd /e/Claude_projects/Emu/NesEmu

cmake -B build/debug   -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
```

### Сборка release
```bash
cmake --build build/release --parallel
```

### Сборка debug
```bash
cmake --build build/debug --parallel
```

## Запуск

```bash
# Release
./build/release/nes_emu.exe

# Debug
./build/debug/nes_emu.exe
```

Или из PowerShell:
```powershell
Start-Process "E:\Claude_projects\Emu\NesEmu\build\release\nes_emu.exe" `
  -WorkingDirectory "E:\Claude_projects\Emu\NesEmu"
```

> **Важно:** запускать из корня проекта (`NesEmu/`), так как программа ищет
> папки `roms/`, `saves/`, `assets/`, `config.json` относительно рабочего каталога.

## ROM файлы

Положите `.nes` файлы в папку `roms/`:
```
NesEmu/
└── roms/
    ├── supermario.nes
    ├── supermario.png   ← обложка (опционально, то же имя)
    └── donkey.nes
```

Поддерживаемые форматы обложек: `.png`, `.jpg`, `.jpeg`

## Управление по умолчанию

| NES кнопка | Клавиша |
|-----------|---------|
| Up        | W       |
| Down      | S       |
| Left      | A       |
| Right     | D       |
| B         | 1       |
| A         | 2       |
| Select    | E       |
| Start     | R       |
| Пауза     | Tab     |

Переназначение кнопок — в меню настроек (кнопка `[S]` на главном экране).

## Тесты

```bash
# Собрать и запустить
cmake --build build/debug --target nes_tests
./build/debug/nes_tests.exe

# Через ctest
ctest --test-dir build/debug -V
```

## nestest (проверка CPU)

Для запуска нужен `roms/nestest.nes` (публично доступный тест-ROM):

```bash
cmake --build build/debug --target nestest

# Только прогон
./build/debug/nestest.exe roms/nestest.nes

# С проверкой по эталонному логу
./build/debug/nestest.exe roms/nestest.nes roms/nestest.log
```

Результат выводится в `nestest_out.log`. `$0002 = 0x00` означает PASS.

## Сохранения

У каждой игры — свои 5 слотов сохранения.  
Файлы хранятся в `saves/{имя_игры}/save_1.sav` … `save_5.sav`.  
Сохранить: `[Tab]` → "Save" → выбрать слот.  
Загрузить: `[Tab]` → "Load" → выбрать слот.
