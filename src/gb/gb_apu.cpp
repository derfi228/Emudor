// gb_apu.cpp — звук Game Boy: 4 канала, секвенсор кадров, микширование.
#include "gb_apu.h"
#include "console/state_io.h"
#include <cmath>

namespace {

constexpr double kCpuHz = 4194304.0;
constexpr double kCyclesPerSample = kCpuHz / GbApu::SAMPLE_RATE;

// Скважность меандра: 12.5%, 25%, 50%, 75%.
constexpr uint8_t kDuty[4][8] = {
    {0,0,0,0,0,0,0,1}, {1,0,0,0,0,0,0,1}, {1,0,0,0,0,1,1,1}, {0,1,1,1,1,1,1,0},
};

// Какие биты регистров читаются единицами (не реализованы в железе).
constexpr uint8_t kReadMask[0x17] = {
    0x80, 0x3F, 0x00, 0xFF, 0xBF,   // NR10-NR14
    0xFF, 0x3F, 0x00, 0xFF, 0xBF,   // NR20-NR24
    0x7F, 0xFF, 0x9F, 0xFF, 0xBF,   // NR30-NR34
    0xFF, 0xFF, 0x00, 0x00, 0xBF,   // NR40-NR44
    0x00, 0x00, 0x70,               // NR50-NR52
};

// Конденсатор на выходе: заряд за один сэмпл.
const float kHpfCharge = (float)std::pow(0.999958, kCyclesPerSample);

} // namespace

// ─── Сброс: состояние после загрузчика ───────────────────────────────────────
void GbApu::reset(bool cgb)
{
    cgb_ = cgb;
    ch_ = {};
    regs_.fill(0);
    power_ = true;
    fsStep_ = 0;
    sampleAcc_ = 0.0; sumOut_ = 0.0; hpfCap_ = 0.0f;
    samples_.clear();
    static const uint8_t kWaveInit[16] = {
        0x84, 0x40, 0x43, 0xAA, 0x2D, 0x78, 0x92, 0x3C, 0x60, 0x59, 0x59, 0xB0, 0x34, 0xB8, 0x2E, 0xDA,
    };
    for (int i = 0; i < 16; ++i) wave_[i] = kWaveInit[i];

    // Регистры, как их оставляет загрузчик (без запуска каналов).
    static const struct { uint16_t a; uint8_t v; } kInit[] = {
        {0xFF10, 0x80}, {0xFF11, 0xBF}, {0xFF12, 0xF3}, {0xFF13, 0xFF}, {0xFF14, 0x3F},
        {0xFF16, 0x3F}, {0xFF17, 0x00}, {0xFF18, 0xFF}, {0xFF19, 0x3F},
        {0xFF1A, 0x7F}, {0xFF1B, 0xFF}, {0xFF1C, 0x9F}, {0xFF1D, 0xFF}, {0xFF1E, 0x3F},
        {0xFF20, 0xFF}, {0xFF21, 0x00}, {0xFF22, 0x00}, {0xFF23, 0x3F},
        {0xFF24, 0x77}, {0xFF25, 0xF3},
    };
    for (const auto& r : kInit) write(r.a, r.v);
    ch_[0].enabled = true;              // звук загрузчика отзвучал, канал 1 числится включённым
}

// ─── Периоды и выходы каналов ────────────────────────────────────────────────
int32_t GbApu::period(int n) const
{
    const Channel& c = ch_[n];
    switch (n) {
    case 0: case 1: return (2048 - c.freq) * 4;
    case 2:         return (2048 - c.freq) * 2;
    default:        return (c.noiseDiv ? c.noiseDiv * 16 : 8) << c.noiseShift;
    }
}

uint8_t GbApu::digital(int n) const
{
    const Channel& c = ch_[n];
    if (!c.enabled || !c.dacOn) return 0;
    switch (n) {
    case 0: case 1: return kDuty[c.duty][c.dutyPos] ? c.volume : 0;
    case 2:         return c.waveVol ? (uint8_t)(c.waveSample >> (c.waveVol - 1)) : 0;
    default:        return (c.lfsr & 1) ? 0 : c.volume;
    }
}

uint8_t GbApu::readPcm(int pair) const
{
    return pair == 0 ? (uint8_t)(digital(0) | (digital(1) << 4))
                     : (uint8_t)(digital(2) | (digital(3) << 4));
}

// ─── Ход времени ─────────────────────────────────────────────────────────────
void GbApu::tick(int dots)
{
    waveJustRead_ = false;
    for (int n = 0; n < 4; ++n) {
        Channel& c = ch_[n];
        if (!c.enabled) continue;
        c.timer -= dots;
        bool fetched = false;
        while (c.timer <= 0) {
            c.timer += period(n);
            fetched = true;
            switch (n) {
            case 0: case 1: c.dutyPos = (uint8_t)((c.dutyPos + 1) & 7); break;
            case 2:
                c.wavePos = (uint8_t)((c.wavePos + 1) & 31);
                c.waveSample = (uint8_t)((c.wavePos & 1) ? (wave_[c.wavePos >> 1] & 0x0F)
                                                          : (wave_[c.wavePos >> 1] >> 4));
                break;
            default: {
                uint16_t bit = (uint16_t)((c.lfsr ^ (c.lfsr >> 1)) & 1);
                c.lfsr = (uint16_t)((c.lfsr >> 1) | (bit << 14));
                if (c.noiseWidth7) c.lfsr = (uint16_t)((c.lfsr & ~0x40) | (bit << 6));
                break;
            }
            }
        }
        // Сэмпл прочитан ровно в последнем такте — окно, в котором DMG
        // пускает процессор к волновой памяти.
        if (n == 2) waveJustRead_ = fetched && c.timer == period(2);
    }

    // Микширование: ЦАП даёт −1..1, NR51 раскладывает каналы по сторонам,
    // NR50 — громкость сторон. Стерео сводим в моно.
    float left = 0.0f, right = 0.0f;
    if (power_) {
        const uint8_t nr51 = regs_[0x15];
        for (int n = 0; n < 4; ++n) {
            if (!ch_[n].dacOn) continue;
            float a = digital(n) / 7.5f - 1.0f;
            if (nr51 & (0x10 << n)) left  += a;
            if (nr51 & (0x01 << n)) right += a;
        }
        const uint8_t nr50 = regs_[0x14];
        left  *= (((nr50 >> 4) & 7) + 1) / 8.0f;
        right *= ((nr50 & 7) + 1) / 8.0f;
    }
    const float mono = (left + right) * 0.125f;   // 4 канала × 2 стороны

    sumOut_ += (double)mono * dots;
    sampleAcc_ += dots;
    if (sampleAcc_ >= kCyclesPerSample) {
        float in = (float)(sumOut_ / sampleAcc_);
        float out = in - hpfCap_;
        hpfCap_ = in - out * kHpfCharge;
        samples_.push_back(out);
        sampleAcc_ -= kCyclesPerSample;
        sumOut_ = (double)mono * sampleAcc_;
    }
}

// ─── Секвенсор кадров: 512 Гц ────────────────────────────────────────────────
void GbApu::frameSequencerStep()
{
    if (!power_) return;
    const bool lengthStep = (fsStep_ & 1) == 0;
    const bool sweepStep  = fsStep_ == 2 || fsStep_ == 6;
    const bool envStep    = fsStep_ == 7;

    if (lengthStep) {
        for (auto& c : ch_) {
            if (c.lengthEnable && c.length > 0 && --c.length == 0) c.enabled = false;
        }
    }
    if (sweepStep) {
        Channel& c = ch_[0];
        if (c.sweepTimer > 0) --c.sweepTimer;
        if (c.sweepTimer == 0) {
            c.sweepTimer = c.sweepPeriod ? c.sweepPeriod : 8;
            if (c.sweepEnabled && c.sweepPeriod) {
                uint16_t nf = sweepCalc();
                if (nf <= 2047 && c.sweepShift) {
                    c.freq = nf;
                    c.sweepShadow = nf;
                    sweepCalc();                   // повторная проверка переполнения
                }
            }
        }
    }
    if (envStep) {
        for (int n : {0, 1, 3}) {
            Channel& c = ch_[n];
            if (c.envPeriod == 0) continue;
            if (c.envTimer > 0) --c.envTimer;
            if (c.envTimer == 0) {
                c.envTimer = c.envPeriod;
                if (c.envUp && c.volume < 15)        ++c.volume;
                else if (!c.envUp && c.volume > 0)   --c.volume;
            }
        }
    }
    fsStep_ = (uint8_t)((fsStep_ + 1) & 7);
}

uint16_t GbApu::sweepCalc()
{
    Channel& c = ch_[0];
    uint16_t delta = (uint16_t)(c.sweepShadow >> c.sweepShift);
    uint16_t nf;
    if (c.sweepNeg) { nf = (uint16_t)(c.sweepShadow - delta); c.sweepNegUsed = true; }
    else            nf = (uint16_t)(c.sweepShadow + delta);
    if (nf > 2047) c.enabled = false;
    return nf;
}

// ─── Запуск канала (бит 7 в NRx4) ────────────────────────────────────────────
void GbApu::trigger(int n)
{
    Channel& c = ch_[n];
    // DMG: перезапуск волны за такт до чтения сэмпла портит начало волновой
    // памяти — туда копируется читаемый байт (или его выровненная четвёрка).
    if (n == 2 && !cgb_ && c.enabled && c.timer <= 2) {
        const int offset = ((c.wavePos + 1) >> 1) & 0x0F;
        if (offset < 4) wave_[0] = wave_[offset];
        else for (int i = 0; i < 4; ++i) wave_[i] = wave_[(offset & ~3) + i];
    }
    c.enabled = c.dacOn;
    if (c.length == 0) {
        c.length = (n == 2) ? 256 : 64;
        // Если следующий шаг секвенсора не считает длительность, а она
        // включена, железо сразу отнимает единицу.
        if (c.lengthEnable && (fsStep_ & 1)) --c.length;
    }
    c.timer = period(n) + (n == 2 ? 6 : 0);   // волна начинает чтение с задержкой
    c.volume = c.envInit;
    c.envTimer = c.envPeriod;
    if (n == 2) c.wavePos = 0;
    if (n == 3) c.lfsr = 0x7FFF;
    if (n == 0) {
        c.sweepShadow = c.freq;
        c.sweepTimer = c.sweepPeriod ? c.sweepPeriod : 8;
        c.sweepEnabled = c.sweepPeriod || c.sweepShift;
        c.sweepNegUsed = false;
        if (c.sweepShift) sweepCalc();
    }
}

void GbApu::powerOff()
{
    for (int i = 0; i < 0x16; ++i) regs_[i] = 0;
    for (auto& c : ch_) {
        uint16_t len = c.length;           // счётчики длительности питание не сбрасывает
        c = Channel{};
        c.length = len;
    }
    power_ = false;
}

// ─── Регистры ─────────────────────────────────────────────────────────────────
uint8_t GbApu::read(uint16_t addr) const
{
    if (addr >= 0xFF30 && addr <= 0xFF3F) {
        // Пока канал 3 играет, доступен только байт, который он сейчас читает
        // (у DMG — лишь в момент чтения, иначе $FF).
        if (ch_[2].enabled) {
            if (!cgb_ && !waveJustRead_) return 0xFF;
            return wave_[ch_[2].wavePos >> 1];
        }
        return wave_[addr - 0xFF30];
    }
    if (addr < 0xFF10 || addr > 0xFF26) return 0xFF;
    const int i = addr - 0xFF10;
    if (addr == 0xFF26) {
        uint8_t v = (uint8_t)(0x70 | (power_ ? 0x80 : 0));
        for (int n = 0; n < 4; ++n) if (ch_[n].enabled) v |= (uint8_t)(1 << n);
        return v;
    }
    return (uint8_t)(regs_[i] | kReadMask[i]);
}

void GbApu::write(uint16_t addr, uint8_t v)
{
    if (addr >= 0xFF30 && addr <= 0xFF3F) {
        if (ch_[2].enabled) {
            if (!cgb_ && !waveJustRead_) return;
            wave_[ch_[2].wavePos >> 1] = v;
            return;
        }
        wave_[addr - 0xFF30] = v;
        return;
    }
    if (addr < 0xFF10 || addr > 0xFF26) return;
    if (addr == 0xFF26) {
        if (!(v & 0x80) && power_) powerOff();
        else if ((v & 0x80) && !power_) {
            power_ = true;
            fsStep_ = 0;
            if (cgb_) for (auto& c : ch_) c.length = 0;   // у CGB счётчики сбрасываются
        }
        return;
    }
    if (!power_) {
        // Выключенный звук принимает только NR52 — но у DMG счётчики
        // длительности (NRx1) остаются доступны для записи.
        if (!cgb_) {
            switch (addr) {
            case 0xFF11: ch_[0].length = (uint16_t)(64 - (v & 0x3F)); break;
            case 0xFF16: ch_[1].length = (uint16_t)(64 - (v & 0x3F)); break;
            case 0xFF1B: ch_[2].length = (uint16_t)(256 - v); break;
            case 0xFF20: ch_[3].length = (uint16_t)(64 - (v & 0x3F)); break;
            default: break;
            }
        }
        return;
    }
    regs_[addr - 0xFF10] = v;

    const int n = (addr - 0xFF10) / 5;      // номер канала
    Channel& c = ch_[n < 4 ? n : 0];
    switch (addr) {
    case 0xFF10: {
        c.sweepPeriod = (uint8_t)((v >> 4) & 7);
        bool wasNeg = c.sweepNeg;
        c.sweepNeg = (v & 0x08) != 0;
        c.sweepShift = (uint8_t)(v & 7);
        if (wasNeg && !c.sweepNeg && c.sweepNegUsed) c.enabled = false;  // причуда железа
        break;
    }
    case 0xFF11: case 0xFF16:
        c.duty = (uint8_t)(v >> 6);
        c.length = (uint16_t)(64 - (v & 0x3F));
        break;
    case 0xFF20:
        c.length = (uint16_t)(64 - (v & 0x3F));
        break;
    case 0xFF1B:
        c.length = (uint16_t)(256 - v);
        break;
    case 0xFF12: case 0xFF17: case 0xFF21:
        c.envInit = (uint8_t)(v >> 4);
        c.envUp = (v & 0x08) != 0;
        c.envPeriod = (uint8_t)(v & 7);
        c.dacOn = (v & 0xF8) != 0;
        if (!c.dacOn) c.enabled = false;
        break;
    case 0xFF1A:
        c.dacOn = (v & 0x80) != 0;
        if (!c.dacOn) c.enabled = false;
        break;
    case 0xFF1C:
        c.waveVol = (uint8_t)((v >> 5) & 3);
        break;
    case 0xFF13: case 0xFF18: case 0xFF1D:
        c.freq = (uint16_t)((c.freq & 0x700) | v);
        break;
    case 0xFF22:
        c.noiseShift = (uint8_t)(v >> 4);
        c.noiseWidth7 = (v & 0x08) != 0;
        c.noiseDiv = (uint8_t)(v & 7);
        break;
    case 0xFF14: case 0xFF19: case 0xFF1E: case 0xFF23: {
        if (addr != 0xFF23) c.freq = (uint16_t)((c.freq & 0xFF) | ((v & 7) << 8));
        const bool wasEnabled = c.lengthEnable;
        c.lengthEnable = (v & 0x40) != 0;
        // Включение счётчика длительности в «нечётной» фазе секвенсора
        // тоже отнимает единицу (так проверяют тестовые ROM звука).
        if (!wasEnabled && c.lengthEnable && (fsStep_ & 1) && c.length > 0) {
            if (--c.length == 0 && !(v & 0x80)) c.enabled = false;
        }
        if (v & 0x80) trigger(n);
        break;
    }
    default: break;                          // NR50/NR51 просто хранятся
    }
}

// ─── Save state ───────────────────────────────────────────────────────────────
template<class S> void GbApu::serialize(S& s)
{
    s.io(ch_); s.io(regs_); s.io(wave_); s.io(power_); s.io(cgb_); s.io(waveJustRead_); s.io(fsStep_);
    s.io(sampleAcc_); s.io(sumOut_); s.io(hpfCap_);
}

template void GbApu::serialize<StateWriter>(StateWriter&);
template void GbApu::serialize<StateReader>(StateReader&);
