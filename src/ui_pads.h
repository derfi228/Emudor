#pragma once
#include <imgui.h>
#include <string>

// ─── Controller pad strip (порт pads.js) ──────────────────────────────────────
// Рисует раскладку кнопок консоли (D-pad/лицевые кнопки/Select-Start) в
// прямоугольник [pos, pos+size], используя внутренний viewBox 320×72 (как в
// оригинальном SVG-макете). consoleId: "NES","SNES","GB","GBA","N64","PS1".
// fg — цвет надписей SELECT/START (берётся из UiTheme::PadTint::fg).
void DrawControllerPad(ImDrawList* dl, ImVec2 pos, ImVec2 size,
                        const std::string& consoleId, ImU32 fg, ImFont* labelFont);
