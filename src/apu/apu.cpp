#include "apu.h"
#include <cmath>

// ─── Таблицы ─────────────────────────────────────────────────────────────────

const uint8_t APU::kLengthTable[32] = {
    10,254, 20,  2, 40,  4, 80,  6, 160,  8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30
};

const uint16_t APU::kNoisePeriods[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};

const uint8_t APU::kDutyTable[4][8] = {
    {0,1,0,0,0,0,0,0},
    {0,1,1,0,0,0,0,0},
    {0,1,1,1,1,0,0,0},
    {1,0,0,1,1,1,1,1},
};

const uint8_t APU::kTriangleTable[32] = {
    15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,
     0, 1, 2, 3, 4, 5,6,7,8,9,10,11,12,13,14,15
};

// ─── Конструктор ─────────────────────────────────────────────────────────────

APU::APU() {
    pulse2_.isChannel2 = true;
}

void APU::reset() {
    // Сброс каналов
    pulse1_   = PulseChannel{};  pulse1_.isChannel2 = false;
    pulse2_   = PulseChannel{};  pulse2_.isChannel2 = true;
    triangle_ = TriangleChannel{};
    noise_    = NoiseChannel{};

    // Сброс счётчиков
    frameCounterMode_ = false;
    irqInhibit_       = false;
    frameCounter_     = 0;
    cpuCycles_        = 0;
    audioAccum_       = 0.0;

    // Сброс фильтров (без резких щелчков на старте)
    hpPrev1_ = hpOut1_ = 0.0f;
    hpPrev2_ = hpOut2_ = 0.0f;
    lpOut_   = 0.0f;

    samples_.clear();
}

void APU::setAudioCallback(std::function<void(float)> cb) {
    audioCallback_ = std::move(cb);
}

// ─── Pulse: envelope / sweep / length / output ───────────────────────────────

void APU::PulseChannel::clockEnvelope() {
    if (envStart) {
        envOutput = 15;
        envTimer  = envPeriod;
        envStart  = false;
    } else if (envTimer > 0) {
        envTimer--;
    } else {
        envTimer = envPeriod;
        if (envOutput > 0)         envOutput--;
        else if (haltFlag)         envOutput = 15;
    }
}

void APU::PulseChannel::clockSweep() {
    if (sweepTimer == 0 && sweepEnabled && timerPeriod >= 8) {
        uint16_t delta = timerPeriod >> sweepShift;
        if (sweepNegate)
            timerPeriod -= isChannel2 ? delta : (delta + 1);
        else
            timerPeriod += delta;
    }
    if (sweepTimer == 0 || sweepReload) {
        sweepTimer  = sweepPeriod;
        sweepReload = false;
    } else {
        sweepTimer--;
    }
}

void APU::PulseChannel::clockLength() {
    if (!haltFlag && lengthVal > 0) lengthVal--;
}

uint8_t APU::PulseChannel::output() const {
    if (!enabled || lengthVal == 0) return 0;
    if (timerPeriod < 8 || timerPeriod > 0x7FF) return 0;
    uint8_t dutyVal = kDutyTable[dutyCycle][sequence_];
    if (!dutyVal) return 0;
    return constVol ? volume : envOutput;
}

// ─── Triangle ────────────────────────────────────────────────────────────────

void APU::TriangleChannel::clockLinear() {
    if (reloadFlag_)       linearCounter = linearPeriod;
    else if (linearCounter > 0) linearCounter--;
    if (!haltFlag)         reloadFlag_ = false;
}

void APU::TriangleChannel::clockLength() {
    if (!haltFlag && lengthVal > 0) lengthVal--;
}

uint8_t APU::TriangleChannel::output() const {
    if (!enabled || lengthVal == 0 || linearCounter == 0) return 0;
    return kTriangleTable[sequence_];
}

// ─── Noise ───────────────────────────────────────────────────────────────────

void APU::NoiseChannel::clockEnvelope() {
    if (envStart) {
        envOutput = 15;
        envTimer  = envPeriod;
        envStart  = false;
    } else if (envTimer > 0) {
        envTimer--;
    } else {
        envTimer = envPeriod;
        if (envOutput > 0)     envOutput--;
        else if (haltFlag)     envOutput = 15;
    }
}

void APU::NoiseChannel::clockLength() {
    if (!haltFlag && lengthVal > 0) lengthVal--;
}

void APU::NoiseChannel::clockTimer() {
    if (timerVal == 0) {
        timerVal = timerPeriod;
        uint16_t fb = mode
            ? ((shift_ >> 6) & 1) ^ (shift_ & 1)
            : ((shift_ >> 1) & 1) ^ (shift_ & 1);
        shift_ = (shift_ >> 1) | (fb << 14);
    } else {
        timerVal--;
    }
}

uint8_t APU::NoiseChannel::output() const {
    if (!enabled || lengthVal == 0 || (shift_ & 1)) return 0;
    return constVol ? volume : envOutput;
}

// ─── Регистры APU ─────────────────────────────────────────────────────────────

uint8_t APU::readStatus() {
    uint8_t val = 0;
    if (pulse1_.lengthVal > 0)   val |= 0x01;
    if (pulse2_.lengthVal > 0)   val |= 0x02;
    if (triangle_.lengthVal > 0) val |= 0x04;
    if (noise_.lengthVal > 0)    val |= 0x08;
    return val;
}

void APU::write(uint16_t addr, uint8_t data) {
    switch (addr) {
    // ── Pulse 1 ($4000–$4003) ──
    case 0x4000:
        pulse1_.dutyCycle = (data >> 6) & 0x03;
        pulse1_.haltFlag  = (data >> 5) & 0x01;
        pulse1_.constVol  = (data >> 4) & 0x01;
        pulse1_.volume    =  data & 0x0F;
        pulse1_.envPeriod =  data & 0x0F;
        break;
    case 0x4001:
        pulse1_.sweepEnabled= (data >> 7) & 1;
        pulse1_.sweepPeriod = (data >> 4) & 0x07;
        pulse1_.sweepNegate = (data >> 3) & 1;
        pulse1_.sweepShift  =  data & 0x07;
        pulse1_.sweepReload = true;
        break;
    case 0x4002:
        pulse1_.timerPeriod = (pulse1_.timerPeriod & 0xFF00) | data;
        break;
    case 0x4003:
        pulse1_.timerPeriod = (pulse1_.timerPeriod & 0x00FF) | ((uint16_t)(data & 0x07) << 8);
        pulse1_.lengthVal   = kLengthTable[data >> 3];
        pulse1_.envStart    = true;
        pulse1_.sequence_   = 0;
        break;

    // ── Pulse 2 ($4004–$4007) ──
    case 0x4004:
        pulse2_.dutyCycle = (data >> 6) & 0x03;
        pulse2_.haltFlag  = (data >> 5) & 0x01;
        pulse2_.constVol  = (data >> 4) & 0x01;
        pulse2_.volume    =  data & 0x0F;
        pulse2_.envPeriod =  data & 0x0F;
        break;
    case 0x4005:
        pulse2_.sweepEnabled= (data >> 7) & 1;
        pulse2_.sweepPeriod = (data >> 4) & 0x07;
        pulse2_.sweepNegate = (data >> 3) & 1;
        pulse2_.sweepShift  =  data & 0x07;
        pulse2_.sweepReload = true;
        break;
    case 0x4006:
        pulse2_.timerPeriod = (pulse2_.timerPeriod & 0xFF00) | data;
        break;
    case 0x4007:
        pulse2_.timerPeriod = (pulse2_.timerPeriod & 0x00FF) | ((uint16_t)(data & 0x07) << 8);
        pulse2_.lengthVal   = kLengthTable[data >> 3];
        pulse2_.envStart    = true;
        pulse2_.sequence_   = 0;
        break;

    // ── Triangle ($4008–$400B) ──
    case 0x4008:
        triangle_.haltFlag    = (data >> 7) & 1;
        triangle_.linearPeriod=  data & 0x7F;
        break;
    case 0x400A:
        triangle_.timerPeriod = (triangle_.timerPeriod & 0xFF00) | data;
        break;
    case 0x400B:
        triangle_.timerPeriod = (triangle_.timerPeriod & 0x00FF) | ((uint16_t)(data & 0x07) << 8);
        triangle_.lengthVal   = kLengthTable[data >> 3];
        triangle_.reloadFlag_ = true;
        break;

    // ── Noise ($400C–$400F) ──
    case 0x400C:
        noise_.haltFlag  = (data >> 5) & 1;
        noise_.constVol  = (data >> 4) & 1;
        noise_.volume    =  data & 0x0F;
        noise_.envPeriod =  data & 0x0F;
        break;
    case 0x400E:
        noise_.mode        = (data >> 7) & 1;
        noise_.timerPeriod = kNoisePeriods[data & 0x0F];
        break;
    case 0x400F:
        noise_.lengthVal = kLengthTable[data >> 3];
        noise_.envStart  = true;
        break;

    // ── Frame counter ($4017) ──
    case 0x4017:
        frameCounterMode_ = (data >> 7) & 1;
        irqInhibit_       = (data >> 6) & 1;
        frameCounter_     = 0;
        if (frameCounterMode_) clockHalfFrame(), clockQuarterFrame();
        break;

    // ── APU status ($4015) ──
    case 0x4015:
        pulse1_.enabled   = (data & 0x01) != 0;
        pulse2_.enabled   = (data & 0x02) != 0;
        triangle_.enabled = (data & 0x04) != 0;
        noise_.enabled    = (data & 0x08) != 0;
        if (!pulse1_.enabled)   pulse1_.lengthVal   = 0;
        if (!pulse2_.enabled)   pulse2_.lengthVal   = 0;
        if (!triangle_.enabled) triangle_.lengthVal = 0;
        if (!noise_.enabled)    noise_.lengthVal    = 0;
        break;
    }
}

// ─── Frame counter ────────────────────────────────────────────────────────────

void APU::clockQuarterFrame() {
    pulse1_.clockEnvelope();
    pulse2_.clockEnvelope();
    triangle_.clockLinear();
    noise_.clockEnvelope();
}

void APU::clockHalfFrame() {
    pulse1_.clockLength();  pulse1_.clockSweep();
    pulse2_.clockLength();  pulse2_.clockSweep();
    triangle_.clockLength();
    noise_.clockLength();
}

void APU::clockFrameCounter() {
    // NTSC: 4-step на ~7457 CPU циклах, 5-step на ~7457/9228
    if (!frameCounterMode_) {
        // 4-step: клоки на 3728, 7456, 11185, 14914
        switch (frameCounter_) {
        case 3728:  clockQuarterFrame(); break;
        case 7456:  clockQuarterFrame(); clockHalfFrame(); break;
        case 11185: clockQuarterFrame(); break;
        case 14914: clockQuarterFrame(); clockHalfFrame();
                    frameCounter_ = static_cast<uint16_t>(-1); break;
        }
    } else {
        // 5-step: клоки на 3728, 7456, 11185, 14914, 18640
        switch (frameCounter_) {
        case 3728:  clockQuarterFrame(); break;
        case 7456:  clockQuarterFrame(); clockHalfFrame(); break;
        case 11185: clockQuarterFrame(); break;
        case 18640: clockQuarterFrame(); clockHalfFrame();
                    frameCounter_ = static_cast<uint16_t>(-1); break;
        }
    }
    frameCounter_++;
}

// ─── Нелинейный микшер ────────────────────────────────────────────────────────

float APU::mixOutput() const {
    float p1 = pulse1_.output();
    float p2 = pulse2_.output();
    float  t = triangle_.output();
    float  n = noise_.output();
    float dmc = 0.0f;

    float pulseOut = 0.0f;
    if (p1 + p2 > 0)
        pulseOut = 95.88f / (8128.0f / (p1 + p2) + 100.0f);

    float tndOut = 0.0f;
    float tnd = t / 8227.0f + n / 12241.0f + dmc / 22638.0f;
    if (tnd > 0)
        tndOut = 159.79f / (1.0f / tnd + 100.0f);

    return pulseOut + tndOut;
}

// ─── Главный такт APU ─────────────────────────────────────────────────────────

void APU::clock() {
    cpuCycles_++;
    clockFrameCounter();

    // Pulse таймеры тикают каждые 2 CPU цикла
    if (cpuCycles_ & 1) {
        if (pulse1_.timerVal == 0) {
            pulse1_.timerVal = pulse1_.timerPeriod;
            pulse1_.sequence_ = (pulse1_.sequence_ + 1) & 0x07;
        } else { pulse1_.timerVal--; }

        if (pulse2_.timerVal == 0) {
            pulse2_.timerVal = pulse2_.timerPeriod;
            pulse2_.sequence_ = (pulse2_.sequence_ + 1) & 0x07;
        } else { pulse2_.timerVal--; }

        noise_.clockTimer();
    }

    // Triangle тикает каждый CPU цикл
    if (triangle_.timerVal == 0) {
        triangle_.timerVal = triangle_.timerPeriod;
        triangle_.sequence_ = (triangle_.sequence_ + 1) & 0x1F;
    } else { triangle_.timerVal--; }

    // Аудио сэмпл: 40.50 циклов/сэмпл = 1786860 / 44100 (60 Hz VSYNC).
    // 40.584 теоретически точнее NES, но при 60 Hz VSYNC даёт 734 сэмпла/кадр
    // против 735, нужных SDL — очередь пустеет за ~12 с → тишина.
    audioAccum_ += 1.0;
    if (audioAccum_ >= 40.50) {
        audioAccum_ -= 40.50;
        float raw = mixOutput();

        // HP1 ≈ 37 Hz: убирает постоянную составляющую (DC offset)
        static constexpr float kHP1 = 0.99474f;
        float hp1 = kHP1 * (hpOut1_ + raw - hpPrev1_);
        hpPrev1_ = raw;
        hpOut1_  = hp1;

        // HP2 ≈ 447 Hz: убирает сверхнизкочастотный гул
        static constexpr float kHP2 = 0.93835f;
        float hp2 = kHP2 * (hpOut2_ + hp1 - hpPrev2_);
        hpPrev2_ = hp1;
        hpOut2_  = hp2;

        // LP ≈ 14 kHz: сглаживает жёсткие квантовые скачки
        static constexpr float kLP = 0.864f;
        float lp = lpOut_ + kLP * (hp2 - lpOut_);
        lpOut_   = lp;

        samples_.push_back(lp);
        if (audioCallback_) audioCallback_(lp);
    }
}
