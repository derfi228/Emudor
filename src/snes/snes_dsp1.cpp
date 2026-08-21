// snes_dsp1.cpp — HLE-эмуляция DSP-1 на уровне команд
#include "snes_dsp1.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>

namespace {

// Угол DSP-1: 16 бит на полный оборот.
inline double angRad(int16_t a) { return (double)a * (6.283185307179586 / 65536.0); }

// Насыщающее приведение к int16_t — железо тоже клампит, а не заворачивает.
inline int16_t sat16(double v)
{
    if (v >  32767.0) return  32767;
    if (v < -32768.0) return -32768;
    return (int16_t)v;
}

// Формат 1.15: 1.0 == 0x8000 (наружу отдаётся до 0x7FFF).
constexpr double kQ15 = 32768.0;
// Матрица Mode 7 — формат 8.8: 1.0 == 0x0100.
constexpr double kQ8  = 256.0;

} // namespace

void SnesDSP1::reset()
{
    command_ = 0;
    inBytes_ = inIndex_ = outBytes_ = outIndex_ = 0;
    std::memset(inBuf_,  0, sizeof inBuf_);
    std::memset(outBuf_, 0, sizeof outBuf_);
    std::memset(mat_,    0, sizeof mat_);
    fx_ = fy_ = fz_ = 0.0;
    lfe_ = 0.0; hgt_ = 0.0; vof_ = 0.0;
    nx_ = 0; ny_ = 0; nz_ = 1;
    gx_ = 0; gy_ = 1; gz_ = 0;
    hx_ = 1; hy_ = 0;
}

// ─── Status Register: вычисления мгновенные, всегда готов ─────────────────────
uint8_t SnesDSP1::readSR() const
{
    return 0x80;
}

// ─── Чтение результата ────────────────────────────────────────────────────────
uint8_t SnesDSP1::readDR()
{
    if (outIndex_ < outBytes_) {
        uint8_t v = outBuf_[outIndex_++];
        if (outIndex_ >= outBytes_) outIndex_ = outBytes_ = 0;
        return v;
    }
    return 0x00;
}

// ─── Запись: сначала байт команды, затем её параметры ─────────────────────────
void SnesDSP1::writeDR(uint8_t v)
{
    if (inBytes_ > 0) {                       // ждём параметры текущей команды
        if (inIndex_ < sizeof inBuf_) inBuf_[inIndex_++] = v;
        if (inIndex_ >= inBytes_) { inBytes_ = 0; exec(); }
        return;
    }

    command_ = v;
    inIndex_ = outIndex_ = outBytes_ = 0;

    uint8_t words = cmdInWords(v);
    if (words == 0xFF) {                      // неизвестная команда — игнорируем
        static const bool dbg = std::getenv("EMUDOR_DSP1") != nullptr;
        if (dbg) std::fprintf(stderr, "DSP1: неизвестная команда %02X\n", v);
        return;
    }
    inBytes_ = (uint8_t)(words * 2);
    if (inBytes_ == 0) exec();                // команда без параметров
}

int16_t SnesDSP1::arg(int i) const
{
    return (int16_t)((uint16_t)inBuf_[i * 2] | ((uint16_t)inBuf_[i * 2 + 1] << 8));
}

void SnesDSP1::setOut(int words)
{
    outBytes_ = (uint8_t)(words * 2);
    outIndex_ = 0;
    std::memset(outBuf_, 0, sizeof outBuf_);
}

void SnesDSP1::put(int i, int16_t v)
{
    outBuf_[i * 2]     = (uint8_t)(v & 0xFF);
    outBuf_[i * 2 + 1] = (uint8_t)((uint16_t)v >> 8);
}

// ─── Таблица команд: сколько слов параметров принимает каждая ─────────────────
// Старшие биты опкода — зеркала (аппаратно выбирают вариант вывода); число
// параметров у зеркал совпадает.
uint8_t SnesDSP1::cmdInWords(uint8_t cmd)
{
    switch (cmd & 0x3F) {
    case 0x00: case 0x20:                       return 2;  // Multiply
    case 0x10: case 0x30:                       return 2;  // Inverse
    case 0x04: case 0x24:                       return 2;  // Triangle
    case 0x08:                                  return 3;  // Radius
    case 0x18: case 0x38:                       return 4;  // Range
    case 0x28:                                  return 3;  // Distance
    case 0x0C: case 0x2C:                       return 3;  // Rotate
    case 0x1C: case 0x3C:                       return 6;  // Polar
    case 0x02: case 0x32:                       return 7;  // Parameter
    case 0x0A: case 0x1A: case 0x2A: case 0x3A: return 1;  // Raster
    case 0x06: case 0x16: case 0x26: case 0x36: return 3;  // Project
    case 0x0E: case 0x1E: case 0x2E: case 0x3E: return 2;  // Target
    case 0x01: case 0x05: case 0x31: case 0x35:
    case 0x11: case 0x15: case 0x21: case 0x25: return 4;  // Attitude A/B/C
    case 0x0D: case 0x1D: case 0x2D: case 0x09:
    case 0x19: case 0x29: case 0x39: case 0x3D: return 3;  // Objective A/B/C
    case 0x03: case 0x13: case 0x23: case 0x33: return 3;  // Subject A/B/C
    case 0x0B: case 0x1B: case 0x2B: case 0x3B: return 3;  // Scalar A/B/C
    case 0x14: case 0x34:                       return 6;  // Gyrate
    case 0x0F: case 0x1F: case 0x2F: case 0x3F:
    case 0x07: case 0x17: case 0x27: case 0x37: return 1;  // самопроверка ПЗУ
    default:                                    return 0xFF;
    }
}

void SnesDSP1::exec()
{
    const uint8_t c = (uint8_t)(command_ & 0x3F);

    // Индекс матрицы для семейств Attitude/Objective/Subject/Scalar:
    // старшая тетрада опкода задаёт вариант A(0)/B(1)/C(2).
    const int mi = ((c & 0x30) >> 4) % 3;

    switch (c) {

    // ── Умножение: результат в формате 1.15 ──────────────────────────────────
    case 0x00: case 0x20: {
        setOut(1);
        put(0, (int16_t)(((int32_t)arg(0) * (int32_t)arg(1)) >> 15));
        break;
    }

    // ── Обратная величина: вход (коэффициент 1.15, порядок) ──────────────────
    case 0x10: case 0x30: {
        setOut(2);
        double coef = (double)arg(0) / kQ15;
        int    e    = arg(1);
        if (coef == 0.0) { put(0, 0x7FFF); put(1, 0x002F); break; }
        double v  = 1.0 / (coef * std::pow(2.0, (double)e));
        int    ie = 0;
        double a  = std::fabs(v);
        while (a >= 1.0) { a *= 0.5; ++ie; }
        while (a <  0.5) { a *= 2.0; --ie; }
        put(0, sat16((v < 0.0 ? -a : a) * kQ15));
        put(1, (int16_t)ie);
        break;
    }

    // ── Треугольник: проекции радиуса на оси ─────────────────────────────────
    case 0x04: case 0x24: {
        double ang = angRad(arg(0));
        double r   = (double)arg(1);
        setOut(2);
        put(0, sat16(r * std::sin(ang)));
        put(1, sat16(r * std::cos(ang)));
        break;
    }

    // ── Квадрат радиуса: 32-битный результат двумя словами ───────────────────
    case 0x08: {
        int32_t x = arg(0), y = arg(1), z = arg(2);
        uint32_t r = (uint32_t)((x * x + y * y + z * z) << 1);
        setOut(2);
        put(0, (int16_t)(r & 0xFFFF));
        put(1, (int16_t)(r >> 16));
        break;
    }

    // ── Дальность: расстояние минус радиус ───────────────────────────────────
    case 0x18: case 0x38: {
        double x = arg(0), y = arg(1), z = arg(2), r = arg(3);
        setOut(1);
        put(0, sat16(std::sqrt(x * x + y * y + z * z) - r));
        break;
    }

    // ── Расстояние до точки ──────────────────────────────────────────────────
    case 0x28: {
        double x = arg(0), y = arg(1), z = arg(2);
        setOut(1);
        put(0, sat16(std::sqrt(x * x + y * y + z * z)));
        break;
    }

    // ── Поворот в плоскости ──────────────────────────────────────────────────
    case 0x0C: case 0x2C: {
        double a = angRad(arg(0));
        double x = arg(1), y = arg(2);
        setOut(2);
        put(0, sat16(x * std::cos(a) + y * std::sin(a)));
        put(1, sat16(y * std::cos(a) - x * std::sin(a)));
        break;
    }

    // ── Поворот в пространстве (три угла) ────────────────────────────────────
    case 0x1C: case 0x3C: case 0x14: case 0x34: {
        double az = angRad(arg(0)), ay = angRad(arg(1)), ax = angRad(arg(2));
        double x  = arg(3), y = arg(4), z = arg(5);
        double x1 = x  * std::cos(az) + y  * std::sin(az);
        double y1 = y  * std::cos(az) - x  * std::sin(az);
        double x2 = x1 * std::cos(ay) - z  * std::sin(ay);
        double z1 = z  * std::cos(ay) + x1 * std::sin(ay);
        double y2 = y1 * std::cos(ax) + z1 * std::sin(ax);
        double z2 = z1 * std::cos(ax) - y1 * std::sin(ax);
        setOut(3);
        put(0, sat16(x2)); put(1, sat16(y2)); put(2, sat16(z2));
        break;
    }

    // ── Parameter: задать камеру, вернуть центр экрана на земле и горизонт ───
    case 0x02: case 0x32: {
        fx_  = arg(0); fy_ = arg(1); fz_ = arg(2);
        lfe_ = arg(3);
        double les = arg(4);
        double aas = angRad(arg(5));       // азимут (поворот вокруг вертикали)
        double azs = angRad(arg(6));       // зенитный угол (отсчёт от вертикали)

        double sa = std::sin(aas), ca = std::cos(aas);
        double sz = std::sin(azs), cz = std::cos(azs);

        // Ортонормированный базис экрана в мировых осях
        nx_ = -sz * sa;  ny_ =  sz * ca;  nz_ =  cz;   // направление взгляда
        gx_ = -cz * sa;  gy_ =  cz * ca;  gz_ = -sz;   // «вверх» по экрану
        hx_ =  ca;       hy_ =  sa;                    // «вправо» по экрану

        // Точка на земле в центре экрана — на расстоянии Les вдоль взгляда.
        // Плоскость земли проходит через неё, значит высота точки обзора над
        // землёй — это Les * Nz (в Fz игра высоту не передаёт, там всегда 0).
        double cx = fx_ + les * nx_;
        double cy = fy_ + les * ny_;
        hgt_ = les * nz_;

        // Горизонт: строка, где луч идёт параллельно земле (Dz = 0)
        double vva = (gz_ != 0.0) ? (-lfe_ * nz_ / gz_) : 0.0;

        vof_ = 0.0;
        setOut(4);
        put(0, sat16(vof_));
        put(1, sat16(vva));
        put(2, sat16(cx));
        put(3, sat16(cy));
        break;
    }

    // ── Raster: матрица Mode 7 для одной строки ──────────────────────────────
    // Луч из точки обзора через строку Vs пересекает землю на расстоянии t;
    // производные точки пересечения по экранным осям и есть A/B/C/D.
    case 0x0A: case 0x1A: case 0x2A: case 0x3A: {
        double v  = (double)arg(0) + vof_;
        double dz = lfe_ * nz_ + v * gz_;
        double t  = (dz != 0.0) ? (hgt_ / dz) : 0.0;
        setOut(4);
        put(0, sat16( t * hx_ * kQ8));   // An = du/dx
        put(1, sat16(-t * gx_ * kQ8));   // Bn = du/dy (экранный y растёт вниз)
        put(2, sat16( t * hy_ * kQ8));   // Cn = dv/dx
        put(3, sat16(-t * gy_ * kQ8));   // Dn = dv/dy
        break;
    }

    // ── Project: точка мира → экранные координаты и масштаб ──────────────────
    case 0x06: case 0x16: case 0x26: case 0x36: {
        double dx = (double)arg(0) - fx_;
        double dy = (double)arg(1) - fy_;
        double dz = (double)arg(2) - fz_;
        double n  = dx * nx_ + dy * ny_ + dz * nz_;   // проекция на взгляд
        setOut(3);
        if (n <= 0.0) {                               // за спиной — не видно
            put(0, 0x7FFF); put(1, 0x7FFF); put(2, 0);
            break;
        }
        double h = dx * hx_ + dy * hy_;
        double g = dx * gx_ + dy * gy_ + dz * gz_;
        put(0, sat16( lfe_ * h / n));
        put(1, sat16(-lfe_ * g / n - vof_));
        put(2, sat16( lfe_ / n * kQ8));
        break;
    }

    // ── Target: экранные координаты → точка на земле ─────────────────────────
    case 0x0E: case 0x1E: case 0x2E: case 0x3E: {
        double h  = arg(0);
        double v  = -(double)arg(1) + vof_;
        double dx = lfe_ * nx_ + h * hx_ + v * gx_;
        double dy = lfe_ * ny_ + h * hy_ + v * gy_;
        double dz = lfe_ * nz_ +           v * gz_;
        double t  = (dz != 0.0) ? (hgt_ / dz) : 0.0;
        setOut(2);
        put(0, sat16(fx_ + t * dx));
        put(1, sat16(fy_ + t * dy));
        break;
    }

    // ── Attitude A/B/C: матрица вращения, умноженная на масштаб ──────────────
    case 0x01: case 0x05: case 0x31: case 0x35:
    case 0x11: case 0x15: case 0x21: case 0x25: {
        double m  = (double)arg(0) / kQ15;
        double az = angRad(arg(1)), ay = angRad(arg(2)), ax = angRad(arg(3));
        double sz = std::sin(az), cz = std::cos(az);
        double sy = std::sin(ay), cy = std::cos(ay);
        double sx = std::sin(ax), cx = std::cos(ax);
        double (*r)[3] = mat_[mi];
        r[0][0] = m * ( cy * cz);
        r[0][1] = m * ( cy * sz);
        r[0][2] = m * (-sy);
        r[1][0] = m * ( sx * sy * cz - cx * sz);
        r[1][1] = m * ( sx * sy * sz + cx * cz);
        r[1][2] = m * ( sx * cy);
        r[2][0] = m * ( cx * sy * cz + sx * sz);
        r[2][1] = m * ( cx * sy * sz - sx * cz);
        r[2][2] = m * ( cx * cy);
        setOut(0);
        break;
    }

    // ── Objective: мир → локальные оси ───────────────────────────────────────
    case 0x0D: case 0x1D: case 0x2D: case 0x09:
    case 0x19: case 0x29: case 0x39: case 0x3D: {
        double x = arg(0), y = arg(1), z = arg(2);
        double (*r)[3] = mat_[mi];
        setOut(3);
        put(0, sat16(r[0][0] * x + r[0][1] * y + r[0][2] * z));
        put(1, sat16(r[1][0] * x + r[1][1] * y + r[1][2] * z));
        put(2, sat16(r[2][0] * x + r[2][1] * y + r[2][2] * z));
        break;
    }

    // ── Subject: локальные оси → мир (транспонированная матрица) ─────────────
    case 0x03: case 0x13: case 0x23: case 0x33: {
        double x = arg(0), y = arg(1), z = arg(2);
        double (*r)[3] = mat_[mi];
        setOut(3);
        put(0, sat16(r[0][0] * x + r[1][0] * y + r[2][0] * z));
        put(1, sat16(r[0][1] * x + r[1][1] * y + r[2][1] * z));
        put(2, sat16(r[0][2] * x + r[1][2] * y + r[2][2] * z));
        break;
    }

    // ── Scalar: проекция вектора на первую ось матрицы ───────────────────────
    case 0x0B: case 0x1B: case 0x2B: case 0x3B: {
        double x = arg(0), y = arg(1), z = arg(2);
        double (*r)[3] = mat_[mi];
        setOut(1);
        put(0, sat16(r[0][0] * x + r[0][1] * y + r[0][2] * z));
        break;
    }

    // ── Самопроверка ПЗУ: игра ждёт нули ─────────────────────────────────────
    default:
        setOut(1);
        put(0, 0);
        break;
    }
}
