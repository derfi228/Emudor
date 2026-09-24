// snes_dsp1.cpp — HLE-эмуляция DSP-1 на уровне команд
#include "snes_dsp1.h"
#include "console/state_io.h"
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
    sa_ = 0; ca_ = 1; sz_ = 0; cz_ = 1;
    nx_ = 0; ny_ = 0; nz_ = 1;
    centreX_ = centreY_ = centreZ_ = 0;
    gx_ = gy_ = gz_ = 0;
    les_ = 0; vOffset_ = 0; secAzs_ = 1;
    rasterVs_ = 0;
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
        if (outIndex_ >= outBytes_) {
            outIndex_ = outBytes_ = 0;
            // Raster работает потоком: игра пишет команду ОДИН раз и читает
            // подряд матрицы всех строк, поэтому после каждой выдачи сразу
            // готовим следующую строку.
            if ((command_ & 0x3F) == 0x0A) { ++rasterVs_; rasterOut(); }
        }
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

    // Пока идёт потоковая выдача Raster, регистр данных 16-битный: игра пишет
    // в него слово $8000 (двумя байтами), и это завершает поток. Разбирать эти
    // байты как команды нельзя — иначе младший $00 защёлкивается как Multiply
    // и весь поток команд уезжает.
    if ((command_ & 0x3F) == 0x0A) {
        if (v == 0x00) return;                // младший байт слова-терминатора
        command_ = 0;                         // любой другой байт закрывает поток
        outBytes_ = outIndex_ = 0;
        if (v == 0x80) return;                // старший байт терминатора
    }

    // Байт команды с установленными битами 7-6 недопустим: железо его не
    // защёлкивает и продолжает ждать команду.
    if (v & 0xC0) return;

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
    // F — точка на земле, куда смотрит камера. Глаз висит над ней на расстоянии
    // Lfe вдоль N = (-sin Azs·sin Aas, sin Azs·cos Aas, cos Azs), экран — на
    // расстоянии Les от глаза. Выход: Vof, Vva (строка горизонта), Cx, Cy
    // (точка земли в центре экрана — при Fz = 0 это сама F).
    case 0x02: case 0x32: {
        const double fx = arg(0), fy = arg(1), fz = arg(2);
        const double lfe = arg(3), les = arg(4);
        const int16_t azsIn = arg(6);
        sa_ = std::sin(angRad(arg(5))); ca_ = std::cos(angRad(arg(5)));
        sz_ = std::sin(angRad(azsIn));  cz_ = std::cos(angRad(azsIn));

        nx_ = -sz_ * sa_;  ny_ = sz_ * ca_;  nz_ = cz_;
        centreX_ = fx + lfe * nx_;           // глаз
        centreY_ = fy + lfe * ny_;
        centreZ_ = fz + lfe * nz_;
        gx_ = centreX_ - les * nx_;          // центр плоскости экрана
        gy_ = centreY_ - les * ny_;
        gz_ = centreZ_ - les * nz_;
        les_ = les;

        // Чип ограничивает зенитный угол (~80°), чтобы горизонт не уходил в
        // бесконечность; порог чуть растёт с высотой глаза.
        static const int16_t kMaxAzs[16] = {
            0x38B4, 0x38B7, 0x38BA, 0x38BE, 0x38C0, 0x38C4, 0x38C7, 0x38CA,
            0x38CE, 0x38D0, 0x38D4, 0x38D7, 0x38DA, 0x38DD, 0x38E0, 0x38E4,
        };
        // Порог выбирается по числу ведущих битов высоты (нормализация 1.15).
        const int16_t z16 = sat16(centreZ_);
        int shift = 0;
        for (int bit = 14; bit >= 0; --bit, ++shift)
            if ((((z16 >> bit) & 1) != 0) != (z16 < 0)) break;
        if (shift > 15) shift = 15;
        int16_t maxAzs = kMaxAzs[shift];
        int16_t azs = azsIn;
        if (azs < 0) { if (azs < -maxAzs + 1) azs = (int16_t)(-maxAzs + 1); }
        else if (azs > maxAzs) azs = maxAzs;
        const double szc = std::sin(angRad(azs)), czc = std::cos(angRad(azs));

        // Точка земли в центре экрана: от глаза вдоль взгляда до земли.
        const double c = (czc != 0.0) ? centreZ_ / czc * szc : 0.0;
        centreX_ += c * sa_;
        centreY_ -= c * ca_;

        vOffset_ = les * czc;
        secAzs_  = (czc != 0.0) ? 1.0 / czc : 32767.0;
        const double vva = (szc != 0.0) ? -vOffset_ / szc : -32768.0;

        setOut(4);
        put(0, 0);                            // Vof: без поправки ограничения угла
        put(1, sat16(vva));
        put(2, sat16(centreX_));
        put(3, sat16(centreY_));
        break;
    }

    // ── Raster: матрица Mode 7 для одной строки ──────────────────────────────
    case 0x0A: case 0x1A: case 0x2A: case 0x3A: {
        rasterVs_ = arg(0);
        rasterOut();
        break;
    }

    // ── Project: точка мира → экранные координаты и масштаб ──────────────────
    // P — вектор от центра плоскости экрана; глубина от глаза = Les - P·N.
    // H, V — проекции P на оси экрана, умноженные на Les/глубина; M — тот же
    // масштаб в формате 8.8.
    case 0x06: case 0x16: case 0x26: case 0x36: {
        const double px = (double)arg(0) - gx_;
        const double py = (double)arg(1) - gy_;
        const double pz = (double)arg(2) - gz_;
        const double depth = les_ - (px * nx_ + py * ny_ + pz * nz_);
        const double k = (depth != 0.0) ? les_ / depth : 32767.0;
        const double h = px * ca_ + py * sa_;
        const double v = px * (-cz_ * sa_) + py * (cz_ * ca_) + pz * (-sz_);
        setOut(3);
        put(0, sat16(h * k));
        put(1, sat16(v * k));
        put(2, sat16(k * kQ8));
        break;
    }

    // ── Target: экранные координаты → точка на земле ─────────────────────────
    case 0x0E: case 0x1E: case 0x2E: case 0x3E: {
        const double h = arg(0), v = arg(1);
        const double den = v * sz_ + vOffset_;
        const double t = (den != 0.0) ? centreZ_ / den : 32767.0;
        setOut(2);
        put(0, sat16(centreX_ + h * t * ca_ - v * t * secAzs_ * sa_));
        put(1, sat16(centreY_ - h * t * sa_ + v * t * secAzs_ * ca_));
        break;
    }

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

// ─── Матрица Mode 7 для строки rasterVs_ ─────────────────────────────────────
// Луч через строку Vs бьёт в землю с масштабом t = высота глаза /
// (Vs·sin Azs + Les·cos Azs). По горизонтали экрана шаг по земле t вдоль
// (cos Aas, sin Aas), по вертикали — t / cos Azs вдоль (-sin Aas, cos Aas).
void SnesDSP1::rasterOut()
{
    const double den = (double)rasterVs_ * sz_ + vOffset_;
    const double t   = (den != 0.0) ? centreZ_ / den : 32767.0;
    const double tv  = t * secAzs_;
    setOut(4);
    put(0, sat16( t  * ca_ * kQ8));   // An
    put(1, sat16(-tv * sa_ * kQ8));   // Bn
    put(2, sat16( t  * sa_ * kQ8));   // Cn
    put(3, sat16( tv * ca_ * kQ8));   // Dn
}


// ─── Save state ───────────────────────────────────────────────────────────────
template<class S> void SnesDSP1::serialize(S& s)
{
    s.io(command_); s.io(inBytes_); s.io(inIndex_); s.io(outBytes_); s.io(outIndex_);
    s.io(inBuf_); s.io(outBuf_);
    s.io(sa_); s.io(ca_); s.io(sz_); s.io(cz_);
    s.io(nx_); s.io(ny_); s.io(nz_);
    s.io(centreX_); s.io(centreY_); s.io(centreZ_);
    s.io(gx_); s.io(gy_); s.io(gz_);
    s.io(les_); s.io(vOffset_); s.io(secAzs_); s.io(rasterVs_);
    s.io(mat_);
}
template void SnesDSP1::serialize<StateWriter>(StateWriter&);
template void SnesDSP1::serialize<StateReader>(StateReader&);
