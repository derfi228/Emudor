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
C:\msys64\usr\bin\bash.exe -lc "export PATH=/mingw64/bin:$PATH && cd /e/Claude_projects/Emu/Emudor && <команда>"
```

### Первая настройка (один раз)
```bash
export PATH=/mingw64/bin:$PATH
cd /e/Claude_projects/Emu/Emudor

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
./build/release/emudor.exe

# Debug
./build/debug/emudor.exe
```

Или из PowerShell:
```powershell
Start-Process "E:\Claude_projects\Emu\Emudor\build\release\emudor.exe" `
  -WorkingDirectory "E:\Claude_projects\Emu\Emudor"
```

> **Важно:** запускать из корня проекта (`Emudor/`), так как программа ищет
> папки `roms/`, `saves/`, `assets/`, `config.json` относительно `SDL_GetBasePath()`
> (директории exe), но рабочий каталог тоже желателен.

## Headless-режим (для автотестов / агентов)

Эмулятор поддерживает запуск без открытия SDL-окна — для CI и ИИ-агентов:

```bash
./build/release/emudor.exe --rom <path> --console NES|SNES \
  --frames <N> --screenshot out.png --headless
```

### Доступные флаги CLI

| Флаг | Назначение |
|---|---|
| `--rom <path>` | Путь к ROM-файлу |
| `--console <NES\|SNES>` | Явный выбор консоли (иначе auto-detect по расширению) |
| `--frames <N>` | Сколько кадров эмулировать перед выходом |
| `--screenshot <path>` | Сохранить финальный кадр как PNG |
| `--screenshot-every <N>` | Сохранять PNG каждые N кадров (нумерация `name_0060.png`) |
| `--headless` | Не открывать SDL2 окно и не запускать ImGui |
| `--record-trace <path>` | Записать per-frame трассу CPU |
| `--help` / `-h` | Справка |

### Коды возврата
- `0` — успех
- `1` — ROM не загрузился / неизвестный флаг
- `2` — краш / неперехваченное исключение

### Примеры

```bash
# Снять скриншот через 600 кадров (10 секунд эмуляции)
./build/release/emudor.exe --headless --rom roms/nes/SMB.nes \
  --frames 600 --screenshot smb_end.png

# Серия скриншотов каждую секунду на 5 секунд (SNES)
./build/release/emudor.exe --headless --rom roms/snes/SMW.sfc \
  --frames 300 --screenshot-every 60 --screenshot smw.png
```

## ROM файлы

Положите `.nes` / `.sfc` файлы в соответствующие подпапки:
```
Emudor/
└── roms/
    ├── nes/
    │   ├── supermario.nes
    │   ├── supermario.png   ← обложка (опционально, то же имя)
    │   └── donkey.nes
    ├── snes/
    │   └── zelda.sfc
    ├── gb/  gbc/  gba/  n64/  ps1/    ← пустые (заготовка)
```

Поддерживаемые форматы обложек: `.png`, `.jpg`, `.jpeg`.

Папка `roms/` сканируется **рекурсивно** — игры в любых подпапках попадают в библиотеку.

## Управление по умолчанию

### NES
| Кнопка | Клавиша |
|---|---|
| Up | W |
| Down | S |
| Left | A |
| Right | D |
| B | 1 |
| A | 2 |
| Select | E |
| Start | R |
| Пауза | Tab |

### SNES (отдельные бинды, настраиваются в Settings)
| Кнопка | Клавиша |
|---|---|
| Up/Down/Left/Right | W/S/A/D |
| B | 1 |
| A | 2 |
| Y | 4 |
| X | 3 |
| L | Q |
| R | T |
| Select | E |
| Start | R |

Переназначение кнопок — в меню настроек (кнопка `[S]` на главном экране).

## Тесты

```bash
# Собрать и запустить
cmake --build build/debug --target nes_tests
./build/debug/nes_tests.exe

# Через ctest
ctest --test-dir build/debug -V
```

## nestest (проверка NES CPU)

Для запуска нужен `roms/nes/nestest.nes` (публично доступный тест-ROM):

```bash
cmake --build build/debug --target nestest

# Только прогон
./build/debug/nestest.exe roms/nes/nestest.nes

# С проверкой по эталонному логу
./build/debug/nestest.exe roms/nes/nestest.nes roms/nes/nestest.log
```

Результат выводится в `nestest_out.log`. `$0002 = 0x00` означает PASS.

## Сохранения

У каждой игры — свои 5 слотов сохранения.
Файлы хранятся в `saves/{имя_игры}/save_1.sav` … `save_5.sav`.
Сохранить: `[Tab]` → "Save" → выбрать слот.
Загрузить: `[Tab]` → "Load" → выбрать слот.
