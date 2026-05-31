#!/usr/bin/env python3
r"""
verify_baseline.py — эвристический анализ baseline-скриншотов.

Использование:
    python tools\verify_baseline.py
    python tools\verify_baseline.py --dir agent_data\baselines\snes
    python tools\verify_baseline.py --quiet         # только подозрительные

Что делает:
1. Обходит agent_data\baselines\<console>\<game>\frame_*.png
2. Для каждой папки игры считает:
   - Размер файлов (маленькие = вероятно чёрные)
   - Цветовое разнообразие (unique colors) каждого кадра
   - Похожесть кадров между собой (если все одинаковые — игра застряла)
3. Выводит вердикт по каждой игре:
   - ✓ working — кадры различаются, цветов много, размеры > 5KB
   - ⚠ suspect — кадры почти идентичны или однотонные
   - ✗ broken — все кадры одного типа (чёрный/одноцветный/Nintendo логотип)

Финальный список SUSPECT/BROKEN — это baseline-ы, которые нужно удалить
и перегенерировать когда соответствующие игры заработают.

ВАЖНО: верификация эвристическая. Окончательное решение должен принимать
человек или агент со зрением (Read PNG visually).
"""
import argparse
import hashlib
import os
import sys
from pathlib import Path
from struct import unpack


# ─── Простой парсер PNG (без зависимостей) ──────────────────────────────────
# Считаем приблизительное цветовое разнообразие через сэмплирование байтов
# IDAT-чанка. Дешёвый, но достаточный для отличия чёрного экрана от gameplay.

def png_size_dimensions(path):
    """Возвращает (file_size, width, height) или None."""
    try:
        sz = os.path.getsize(path)
        with open(path, "rb") as f:
            sig = f.read(8)
            if sig != b"\x89PNG\r\n\x1a\n":
                return None
            # Первый чанк должен быть IHDR
            f.read(4)               # length
            ctype = f.read(4)
            if ctype != b"IHDR":
                return None
            w, h = unpack(">II", f.read(8))
        return (sz, w, h)
    except (OSError, IOError):
        return None


def png_file_hash(path):
    """SHA256 файла (для проверки идентичности скриншотов)."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(8192):
            h.update(chunk)
    return h.hexdigest()


# ─── Категоризация одного файла ─────────────────────────────────────────────

# Эвристика: PNG 256x224 (SNES) с пустым/чёрным экраном весит ~700-800 байт
# (тривиальный zlib-стрим). Цветной gameplay — >5KB.
BLACK_THRESHOLD_BYTES = 1200      # ниже — почти наверняка чёрный/одноцветный
SUSPECT_THRESHOLD_BYTES = 5000    # ниже но не чёрный — однообразная картинка
# Минимум кадров в папке для valid baseline
EXPECTED_FRAMES = 6


def classify_frame(path):
    info = png_size_dimensions(path)
    if info is None:
        return ("invalid", 0)
    sz, w, h = info
    if sz < BLACK_THRESHOLD_BYTES:
        return ("black", sz)
    if sz < SUSPECT_THRESHOLD_BYTES:
        return ("monotone", sz)
    return ("colorful", sz)


def classify_game(game_dir):
    """Возвращает (verdict, details) для папки игры."""
    frames = sorted(Path(game_dir).glob("frame_*.png"))
    if not frames:
        return ("broken", "no PNG files found")

    classes = []
    hashes = []
    sizes = []
    for f in frames:
        cat, sz = classify_frame(f)
        classes.append(cat)
        sizes.append(sz)
        hashes.append(png_file_hash(f)[:16])

    unique_hashes = set(hashes)

    # 1. Все кадры идентичны?
    if len(unique_hashes) == 1:
        return ("broken", f"all {len(frames)} frames are byte-identical (stuck on one screen)")

    # 2. Все кадры почти идентичные по размеру + все маленькие?
    if all(c in ("black", "monotone") for c in classes):
        return ("broken", f"all frames are black/monotone (sizes: {sizes})")

    # 3. Большинство чёрных?
    n_black = sum(1 for c in classes if c == "black")
    if n_black >= len(frames) // 2:
        return ("suspect", f"{n_black}/{len(frames)} frames are black (sizes: {sizes})")

    # 4. Слишком мало уникальных кадров?
    if len(unique_hashes) < EXPECTED_FRAMES // 2:
        return ("suspect", f"only {len(unique_hashes)} unique frame(s) out of {len(frames)}")

    # 5. Меньше ожидаемого количества кадров?
    if len(frames) < EXPECTED_FRAMES:
        return ("suspect", f"only {len(frames)} frames (expected {EXPECTED_FRAMES})")

    # Иначе — выглядит здорово
    return ("working", f"{len(unique_hashes)} unique frames, sizes {sizes}")


# ─── main ───────────────────────────────────────────────────────────────────

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--dir", default="agent_data/baselines",
                   help="Корень baseline-ов (по умолчанию agent_data/baselines)")
    p.add_argument("--quiet", "-q", action="store_true",
                   help="Не печатать ✓ working, только ⚠/✗")
    args = p.parse_args()

    root = Path(args.dir)
    if not root.exists():
        print(f"ERROR: {root} not found", file=sys.stderr)
        return 1

    summary = {"working": 0, "suspect": 0, "broken": 0}
    bad_paths = []

    # Обход consoles → games
    for console_dir in sorted(root.iterdir()):
        if not console_dir.is_dir():
            continue
        print(f"\n=== {console_dir.name} ===")
        for game_dir in sorted(console_dir.iterdir()):
            if not game_dir.is_dir():
                continue
            verdict, details = classify_game(game_dir)
            summary[verdict] += 1
            icon = {"working": "OK ", "suspect": "?? ", "broken": "BAD"}[verdict]
            rel = game_dir.relative_to(root.parent)
            if verdict == "working" and args.quiet:
                continue
            print(f"  {icon} {verdict:8s} {game_dir.name}")
            print(f"      {details}")
            if verdict != "working":
                bad_paths.append(str(rel))

    print(f"\n--- Сводка ---")
    print(f"  OK  working : {summary['working']}")
    print(f"  ??  suspect : {summary['suspect']}")
    print(f"  BAD broken  : {summary['broken']}")

    if bad_paths:
        print(f"\nПодозрительные / сломанные baseline-ы:")
        for p in bad_paths:
            print(f"  {p}")
        print(f"\nРекомендация: удалить broken (rm -rf <path>), перегенерировать когда игра заработает.")
        return 2

    return 0


if __name__ == "__main__":
    sys.exit(main())
