#include "ui_pads.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

// ─── Трансформация viewBox (320×72) → экранный прямоугольник ─────────────────
struct Xf {
    ImVec2 origin;
    float  sx, sy;
    ImVec2 operator()(float x, float y) const { return { origin.x + x * sx, origin.y + y * sy }; }
    float  s() const { return (sx + sy) * 0.5f; }
};

ImU32 PadCol(uint32_t rgb, uint8_t a = 255) {
    return IM_COL32((rgb>>16)&0xFF, (rgb>>8)&0xFF, rgb&0xFF, a);
}

void FilledCircle(ImDrawList* dl, const Xf& xf, float cx, float cy, float r,
                   ImU32 fill, ImU32 stroke) {
    ImVec2 c = xf(cx, cy);
    float  rr = r * xf.s();
    dl->AddCircleFilled(c, rr, fill, 20);
    if (stroke) dl->AddCircle(c, rr, stroke, 20, 1.2f);
}

void Letter(ImDrawList* dl, const Xf& xf, ImFont* font, float cx, float cy,
            float sizeV, const char* ch, ImU32 col) {
    if (!font || !ch || !*ch) return;
    float px = sizeV * xf.s() * 1.6f;
    if (px < 6.0f) return;
    ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0.0f, ch);
    ImVec2 p  = xf(cx, cy);
    dl->AddText(font, px, { p.x - sz.x * 0.5f, p.y - sz.y * 0.5f }, col, ch);
}

// Плюсообразный D-pad. ВАЖНО: крест — НЕВЫПУКЛАЯ фигура (вогнутые углы между
// плечами), а ImDrawList::AddConvexPolyFilled корректно работает только с
// выпуклыми многоугольниками — на невыпуклом даёт визуальные искажения
// ("кривые крестовины"). Поэтому собираем крест из 5 прямоугольников
// (центр + 4 плеча), каждый гарантированно выпуклый.
void DPad(ImDrawList* dl, const Xf& xf, float cx, float cy, float scale, ImU32 color) {
    const float a = 4.4f * scale;   // полуширина плеча
    const float L = 14.0f * scale;  // длина от центра до кончика плеча
    dl->AddRectFilled(xf(cx-a, cy-a), xf(cx+a, cy+a), color);   // центр
    dl->AddRectFilled(xf(cx-a, cy-L), xf(cx+a, cy-a), color);   // вверх
    dl->AddRectFilled(xf(cx-a, cy+a), xf(cx+a, cy+L), color);   // вниз
    dl->AddRectFilled(xf(cx-L, cy-a), xf(cx-a, cy+a), color);   // влево
    dl->AddRectFilled(xf(cx+a, cy-a), xf(cx+L, cy+a), color);   // вправо

    ImVec2 v[12] = {
        xf(cx-a, cy-L), xf(cx+a, cy-L), xf(cx+a, cy-a), xf(cx+L, cy-a),
        xf(cx+L, cy+a), xf(cx+a, cy+a), xf(cx+a, cy+L), xf(cx-a, cy+L),
        xf(cx-a, cy+a), xf(cx-L, cy+a), xf(cx-L, cy-a), xf(cx-a, cy-a),
    };
    dl->AddPolyline(v, 12, IM_COL32(10,8,5,140), ImDrawFlags_Closed, 1.0f);
}

// Повёрнутая "пилюля" (SELECT/START), приближение — прямоугольник с круглыми
// колпачками по краям, повёрнутый на angleDeg вокруг (cx,cy).
void Pill(ImDrawList* dl, const Xf& xf, float cx, float cy, float w, float h,
          float angleDeg, ImU32 fill) {
    float rad = angleDeg * 3.14159265f / 180.0f;
    float cs = std::cos(rad), sn = std::sin(rad);
    auto rot = [&](float lx, float ly) {
        return xf(cx + lx*cs - ly*sn, cy + lx*sn + ly*cs);
    };
    float hw = w * 0.5f, hh = h * 0.5f;
    ImVec2 v[4] = { rot(-hw,-hh), rot(hw,-hh), rot(hw,hh), rot(-hw,hh) };
    dl->AddConvexPolyFilled(v, 4, fill);
    float capR = hh * xf.s();
    dl->AddCircleFilled(rot(-hw, 0.0f), capR, fill, 12);
    dl->AddCircleFilled(rot( hw, 0.0f), capR, fill, 12);
}

void ChipLabel(ImDrawList* dl, const Xf& xf, float x, float y, float w, float h,
               ImU32 fill) {
    ImVec2 p0 = xf(x, y), p1 = xf(x+w, y+h);
    dl->AddRectFilled(p0, p1, fill, (p1.y-p0.y)*0.35f);
}

void PS1Arrow(ImDrawList* dl, const Xf& xf, float cx, float cy, int dir, ImU32 fill) {
    // dir: 0=up,1=down,2=left,3=right — пятиугольная стрелка (приближение
    // треугольником + прямоугольным хвостом, форма угадывается на малом масштабе).
    ImVec2 tri[5];
    switch (dir) {
    case 0: tri[0]=xf(cx-4,cy-1); tri[1]=xf(cx+4,cy-1); tri[2]=xf(cx+4,cy-6);
            tri[3]=xf(cx,   cy-10); tri[4]=xf(cx-4,cy-6); break;
    case 1: tri[0]=xf(cx-4,cy+1); tri[1]=xf(cx+4,cy+1); tri[2]=xf(cx+4,cy+6);
            tri[3]=xf(cx,   cy+10); tri[4]=xf(cx-4,cy+6); break;
    case 2: tri[0]=xf(cx-1,cy-4); tri[1]=xf(cx-1,cy+4); tri[2]=xf(cx-6,cy+4);
            tri[3]=xf(cx-10,cy);  tri[4]=xf(cx-6,cy-4); break;
    default:tri[0]=xf(cx+1,cy-4); tri[1]=xf(cx+1,cy+4); tri[2]=xf(cx+6,cy+4);
            tri[3]=xf(cx+10,cy);  tri[4]=xf(cx+6,cy-4); break;
    }
    dl->AddConvexPolyFilled(tri, 5, fill);
}

void PS1Shape(ImDrawList* dl, const Xf& xf, float cx, float cy, ImU32 base, ImU32 glyph, char kind) {
    FilledCircle(dl, xf, cx, cy, 7.0f, base, 0);
    ImVec2 c = xf(cx, cy);
    float s = xf.s();
    switch (kind) {
    case 't': { // треугольник (Δ)
        ImVec2 p0 = xf(cx, cy-4), p1 = xf(cx-3.6f, cy+2.4f), p2 = xf(cx+3.6f, cy+2.4f);
        dl->AddTriangle(p0, p1, p2, glyph, 1.7f*s);
        break;
    }
    case 's': { // квадрат (□)
        ImVec2 p0 = xf(cx-3.4f, cy-3.4f), p1 = xf(cx+3.4f, cy+3.4f);
        dl->AddRect(p0, p1, glyph, 0, 0, 1.7f*s);
        break;
    }
    case 'c': // круг (○)
        dl->AddCircle(c, 3.4f*s, glyph, 16, 1.7f*s);
        break;
    default: { // крест (✕)
        ImVec2 p0 = xf(cx-3.4f, cy-3.4f), p1 = xf(cx+3.4f, cy+3.4f);
        ImVec2 p2 = xf(cx+3.4f, cy-3.4f), p3 = xf(cx-3.4f, cy+3.4f);
        dl->AddLine(p0, p1, glyph, 1.9f*s);
        dl->AddLine(p2, p3, glyph, 1.9f*s);
        break;
    }
    }
}

} // namespace

void DrawControllerPad(ImDrawList* dl, ImVec2 pos, ImVec2 size,
                        const std::string& consoleId, ImU32 fg, ImFont* labelFont) {
    // Тёмный "пластик" для D-pad/корпусных элементов — единый на все консоли,
    // как в pads.js (#2a2620 / #0c0620 для GBA).
    const ImU32 plastic = PadCol(0x2a2620);

    // ── Колонка с названием консоли — фиксированные 64px (CSS grid-template-
    // columns: 64px 1fr, не пропорция от ширины карточки) ─────────────────────
    const float kLabelColPx = 64.0f;
    const float kGapPx      = 10.0f;
    float labelW = std::min(kLabelColPx, size.x * 0.5f);  // защита для очень узких карточек
    if (labelFont) {
        std::string up = consoleId;
        for (auto& c : up) c = (char)toupper((unsigned char)c);
        float fsz = std::min(22.0f, size.y * 0.62f);
        ImVec2 tsz = labelFont->CalcTextSizeA(fsz, FLT_MAX, 0.0f, up.c_str());
        ImVec2 tp = { pos.x + (labelW - tsz.x) * 0.5f, pos.y + (size.y - tsz.y) * 0.5f };
        dl->AddText(labelFont, fsz, tp, fg, up.c_str());
    }
    labelW += kGapPx;

    // ── Область самого пэда: viewBox 320×72 → оставшийся прямоугольник ────────
    ImVec2 padPos  = { pos.x + labelW, pos.y };
    ImVec2 padSize = { size.x - labelW, size.y };
    if (padSize.x <= 1.0f) return;
    Xf xf{ padPos, padSize.x / 320.0f, padSize.y / 72.0f };

    if (consoleId == "NES") {
        DPad(dl, xf, 50, 36, 1.05f, plastic);
        // SELECT / START — тёмные прямоугольники
        ImVec2 s0 = xf(132,31), s1 = xf(154,41);
        dl->AddRectFilled(s0, s1, plastic, 2.0f*xf.s());
        ImVec2 t0 = xf(166,31), t1 = xf(188,41);
        dl->AddRectFilled(t0, t1, plastic, 2.0f*xf.s());
        FilledCircle(dl, xf, 248, 36, 12, PadCol(0xc41f24), PadCol(0x5a0c0c));
        FilledCircle(dl, xf, 280, 36, 12, PadCol(0xc41f24), PadCol(0x5a0c0c));
        Letter(dl, xf, labelFont, 248, 36, 5.5f, "B", IM_COL32(255,255,255,235));
        Letter(dl, xf, labelFont, 280, 36, 5.5f, "A", IM_COL32(255,255,255,235));
    } else if (consoleId == "SNES") {
        ChipLabel(dl, xf, 14, 6, 40, 8, plastic);
        ChipLabel(dl, xf, 266, 6, 40, 8, plastic);
        DPad(dl, xf, 50, 40, 1.0f, plastic);
        Pill(dl, xf, 138, 36, 22, 7.2f, -22.0f, plastic);
        Pill(dl, xf, 180, 44, 22, 7.2f, -22.0f, plastic);
        FilledCircle(dl, xf, 266, 26, 7.4f, PadCol(0x2d6cb6), PadCol(0x0e3360)); // X
        FilledCircle(dl, xf, 252, 40, 7.4f, PadCol(0x3aa257), PadCol(0x13502a)); // Y
        FilledCircle(dl, xf, 280, 40, 7.4f, PadCol(0xd23a3a), PadCol(0x601515)); // A
        FilledCircle(dl, xf, 266, 54, 7.4f, PadCol(0xe9b73a), PadCol(0x7a5a0e)); // B
        Letter(dl, xf, labelFont, 266, 26, 4.6f, "X", IM_COL32(255,255,255,230));
        Letter(dl, xf, labelFont, 252, 40, 4.6f, "Y", IM_COL32(255,255,255,230));
        Letter(dl, xf, labelFont, 280, 40, 4.6f, "A", IM_COL32(255,255,255,230));
        Letter(dl, xf, labelFont, 266, 54, 4.6f, "B", IM_COL32(58,42,0,230));
    } else if (consoleId == "GB") {
        DPad(dl, xf, 50, 36, 1.05f, plastic);
        Pill(dl, xf, 138, 40, 20, 6, -22.0f, plastic);
        Pill(dl, xf, 174, 46, 20, 6, -22.0f, plastic);
        FilledCircle(dl, xf, 252, 36, 11, PadCol(0xa31e54), PadCol(0x3a0a18));
        FilledCircle(dl, xf, 280, 36, 11, PadCol(0xa31e54), PadCol(0x3a0a18));
        Letter(dl, xf, labelFont, 252, 36, 5.5f, "B", IM_COL32(255,255,255,230));
        Letter(dl, xf, labelFont, 280, 36, 5.5f, "A", IM_COL32(255,255,255,230));
    } else if (consoleId == "GBA") {
        ImU32 indigo = PadCol(0x0c0620);
        ChipLabel(dl, xf, 14, 6, 40, 8, indigo);
        ChipLabel(dl, xf, 266, 6, 40, 8, indigo);
        DPad(dl, xf, 50, 40, 1.0f, indigo);
        Pill(dl, xf, 143, 40, 18, 6, 0.0f, indigo);
        Pill(dl, xf, 177, 40, 18, 6, 0.0f, indigo);
        FilledCircle(dl, xf, 252, 36, 11, PadCol(0xa31e54), PadCol(0x3a0a18));
        FilledCircle(dl, xf, 280, 36, 11, PadCol(0xa31e54), PadCol(0x3a0a18));
        Letter(dl, xf, labelFont, 252, 36, 5.5f, "B", IM_COL32(255,255,255,230));
        Letter(dl, xf, labelFont, 280, 36, 5.5f, "A", IM_COL32(255,255,255,230));
    } else if (consoleId == "N64") {
        DPad(dl, xf, 34, 36, 0.85f, plastic);
        FilledCircle(dl, xf, 98, 36, 18, plastic, 0);
        FilledCircle(dl, xf, 98, 36, 13, PadCol(0x88826e), 0);
        FilledCircle(dl, xf, 98, 36, 9,  PadCol(0x5d5847), 0);
        FilledCircle(dl, xf, 146, 36, 6, PadCol(0xc83030), PadCol(0x5a0c0c));
        FilledCircle(dl, xf, 196, 38, 11, PadCol(0x2d6cb6), PadCol(0x0e3360));
        FilledCircle(dl, xf, 178, 56, 7,  PadCol(0x3aa257), PadCol(0x13502a));
        Letter(dl, xf, labelFont, 196, 38, 5.5f, "A", IM_COL32(255,255,255,230));
        Letter(dl, xf, labelFont, 178, 56, 4.2f, "B", IM_COL32(255,255,255,230));
        FilledCircle(dl, xf, 266, 22, 5.4f, PadCol(0xe9b526), PadCol(0x7a5a0c));
        FilledCircle(dl, xf, 252, 36, 5.4f, PadCol(0xe9b526), PadCol(0x7a5a0c));
        FilledCircle(dl, xf, 280, 36, 5.4f, PadCol(0xe9b526), PadCol(0x7a5a0c));
        FilledCircle(dl, xf, 266, 50, 5.4f, PadCol(0xe9b526), PadCol(0x7a5a0c));
    } else if (consoleId == "PS1") {
        ChipLabel(dl, xf, 14, 6, 40, 8, plastic);
        ChipLabel(dl, xf, 266, 6, 40, 8, plastic);
        PS1Arrow(dl, xf, 54, 29, 0, plastic);
        PS1Arrow(dl, xf, 54, 51, 1, plastic);
        PS1Arrow(dl, xf, 43, 40, 2, plastic);
        PS1Arrow(dl, xf, 65, 40, 3, plastic);
        ImVec2 s0 = xf(138,37), s1 = xf(148,43);
        dl->AddRectFilled(s0, s1, plastic, 1.0f*xf.s());
        dl->AddTriangleFilled(xf(172,37), xf(182,40), xf(172,43), plastic);
        PS1Shape(dl, xf, 266, 26, plastic, PadCol(0x3aa257), 't');
        PS1Shape(dl, xf, 252, 40, plastic, PadCol(0xd97aa0), 's');
        PS1Shape(dl, xf, 280, 40, plastic, PadCol(0xd23a3a), 'c');
        PS1Shape(dl, xf, 266, 54, plastic, PadCol(0x2d6cb6), 'x');
    }
}
