// snes_console.cpp — реализация SnesConsole
#include "snes_console.h"
#include <ostream>
#include <istream>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cstdlib>

// ─── Тайминг SNES (NTSC) ─────────────────────────────────────────────────────
// Мастер-такт: 21.477272 МГц
// CPU: каждые ~6 мастер-тактов (≈3,579 МГц)
// PPU: каждые ~4 мастер-тактов (≈5,369 МГц)
// APU (SPC700): ~24 мастер-тактов (≈1,024 МГц)
//
// Для простоты эмулируем так:
//   1 «главный шаг» = 1 PPU дот
//   CPU  шагает каждые 2 PPU дота (пессимистично)
//   APU  шагает каждые 21 PPU дота
//
// Одного кадра: 341 дот × 262 сканлайна = 89342 PPU доти

SnesConsole::SnesConsole()
{
    bus_.connectPPU(&ppu_);
    bus_.connectAPU(&apu_);
    cpu_.connectBus(&bus_);
    ppu_.connectBus(&bus_);
}

bool SnesConsole::loadROM(const std::string& path)
{
    if (!bus_.loadROM(path)) return false;
    reset();
    return true;
}

void SnesConsole::reset()
{
    bus_.reset();
    ppu_.reset();
    apu_.reset();
    cpu_.reset();
    audioF_.clear();
    masterClock_ = 0;
    spcNextTick_ = 0;
    resamplePos_ = 0.0;
    lastMono_    = 0.0f;
}

// ─── Один кадр ───────────────────────────────────────────────────────────────
// NTSC SNES: 341 дота × 262 сканлайна = 89342 PPU-дота на кадр.
// Цикл ВСЕГДА выполняется полностью (включая VBlank 225–261), чтобы
// NMI-обработчик успел отработать до начала следующего активного кадра.
void SnesConsole::runFrame()
{
    using namespace snes_timing;

    bus_.setVBlankActive(false);

    int cpuAcc = 0;

    // ── Cycle-accurate co-scheduler ──────────────────────────────────────────
    // PPU тикаем по доту, CPU — раз в 2 дота (как раньше, его тайминг НЕ меняем),
    // а SPC700 интерливим по общей шкале мастер-тактов: пока следующая
    // инструкция SPC «созрела» (spcNextTick_ <= masterClock_) — исполняем её.
    // Так SPC работает ОДНОВРЕМЕННО с CPU, и хендшейк через порты идёт с верным
    // относительным таймингом — это и устраняет дедлок загрузки N-SPC.
    for (int dot = 0; dot < DOTS_PER_FRAME; ++dot) {
        // ── PPU (1 дот) ──────────────────────────────────────────────────────
        ppu_.clock();
        masterClock_ += MASTER_PER_PPU_DOT;

        // ── VBlank / NMI ─────────────────────────────────────────────────────
        if (ppu_.nmiPending) {
            ppu_.nmiPending = false;
            bus_.setVBlankActive(true);   // $4212 bit7=1
            bus_.setNmiFlag(true);        // $4210 bit7=1
            bus_.resetHDMA();             // реинициализировать HDMA каждый VBlank
            bus_.latchAutoJoy();          // авто-опрос джойпада в начале VBlank
            // WAI просыпается от любого NMI-сигнала, независимо от $4200.7
            cpu_.waiting_ = false;
            // NMI только если $4200 bit7=1 (NMITIMEN)
            if (bus_.nmiEnabled()) cpu_.nmi();
        }

        // ── CPU: 1 инструкция каждые 2 PPU дота ──────────────────────────────
        if (++cpuAcc >= 2) {
            cpuAcc = 0;
            if (!cpu_.stopped_ && !cpu_.waiting_) cpu_.clock();
        }

        // ── SPC700: интерлив по мастер-такту (конкурентно с CPU) ─────────────
        while (spcNextTick_ <= masterClock_) {
            int c = apu_.stepOne();                              // одна инструкция SPC
            spcNextTick_ += (uint64_t)c * MASTER_PER_SPC_CYCLE;  // её длительность
            apu_.tickDsp(c);                                     // DSP-сэмплы (~32 кГц)
        }

        // ── HDMA / IRQ: раз в сканлайн ─────────────────────────────────────────
        // Строку 0 тоже проверяем: игра может ставить цель IRQ на неё
        // (Mario Kart в заезде просит H+V IRQ при vTarget = 0).
        if (dot % 341 == 0) {
            int scanline = dot / 341;

            // SuperFX/GSU работает параллельно CPU (~3:1 по тактам)
            bus_.runSuperFX(256);

            // HDMA шагает ТОЛЬКО по видимым строкам (1–224). В VBlank его гонять
            // нельзя: resetHDMA() на NMI (строка 225) уже выставил hdmaInit_, и
            // прогон в VBlank пере-инициализировал бы канал и «съел» начало
            // таблицы → эффект съезжал по вертикали, низ экрана ломался
            // (арена Street Fighter II, дождь Zelda, фон EarthBound).
            if (scanline < 225) bus_.runHDMA();

            // ── IRQ по V-таймеру (только режимы 10/11 $4200) ──────────────────
            // H-IRQ намеренно не реализован — он срабатывает на каждой строке
            // и легко флудит CPU. Большинство игр используют V-IRQ.
            uint8_t mode = bus_.irqMode();
            if (mode == 2 || mode == 3) {
                if (scanline == (int)bus_.vTarget()) {
                    bus_.raiseIrq();
                    cpu_.waiting_ = false;
                    cpu_.irq();
                }
            }
        }
    }

    // VBlank завершился, сбрасываем флаг
    bus_.setVBlankActive(false);

    // ─── Диагностика (все getenv кэшированы в static — вызов раз на процесс) ──
    static const bool s_apuDbg    = std::getenv("EMUDOR_APU_DBG")    != nullptr;
    static const bool s_cpuTrace  = std::getenv("EMUDOR_CPU_TRACE")  != nullptr;
    static const bool s_stateDump = std::getenv("EMUDOR_STATE_DUMP") != nullptr;

    // ─── Диагностика хендшейка N-SPC (EMUDOR_APU_DBG) ───────────────────────
    if (s_apuDbg) {
        fprintf(stderr, "f=%d spcPC=%04X F1=%02X | OUT %02X %02X %02X %02X | IN %02X %02X %02X %02X | $04/$05=%02X %02X | cpuPC=%04X\n",
            dbgFrames_, apu_.dbgSpcPC(), apu_.dbgF1(),
            apu_.dbgPort(0), apu_.dbgPort(1), apu_.dbgPort(2), apu_.dbgPort(3),
            apu_.dbgPortIn(0), apu_.dbgPortIn(1), apu_.dbgPortIn(2), apu_.dbgPortIn(3),
            apu_.dbgRam(0x04), apu_.dbgRam(0x05), cpu_.PC);
    }

    // ─── Диагностика: трассировка CPU PC каждый кадр (EMUDOR_CPU_TRACE) ──────
    if (s_cpuTrace) {
        fprintf(stderr, "f=%d PBR:PC=%02X:%04X A=%04X X=%04X Y=%04X SP=%04X P=%02X wait=%d stop=%d\n",
                dbgFrames_, cpu_.PBR, cpu_.PC, cpu_.A, cpu_.X, cpu_.Y, cpu_.SP, cpu_.P,
                (int)cpu_.waiting_, (int)cpu_.stopped_);
    }

    // ─── Диагностика: одноразовый дамп состояния после 180 кадров ────────────
    if (++dbgFrames_ == 180 && s_stateDump) {
        if (FILE* f = fopen("debug_snes.txt", "w")) {
            fprintf(f, "=== SNES state after 180 frames ===\n");
            fprintf(f, "CPU: PBR=%02X PC=%04X A=%04X X=%04X Y=%04X SP=%04X P=%02X E=%d\n",
                cpu_.PBR, cpu_.PC, cpu_.A, cpu_.X, cpu_.Y, cpu_.SP, cpu_.P, (int)cpu_.E);
            fprintf(f, "CPU: waiting=%d stopped=%d\n", (int)cpu_.waiting_, (int)cpu_.stopped_);
            fprintf(f, "SPC: PC=%04X portOut[0]=%02X portOut[1]=%02X\n",
                apu_.dbgSpcPC(), apu_.dbgPort(0), apu_.dbgPort(1));
            fclose(f);
        }
    }

    flushAudio();
}

void SnesConsole::flushAudio()
{
    // DSP выдаёт стерео int16 @ ~32 кГц (L R L R …). SDL-устройство приложения —
    // 44100 Гц МОНО float (как у NES). Здесь: стерео→моно (среднее) и ресэмпл
    // 32000→44100 с ЛИНЕЙНОЙ интерполяцией (мягче, без «хруста» nearest-neighbour).
    // Фаза resamplePos_ сохраняется между кадрами; последний сэмпл прошлого кадра
    // (lastMono_) используется для интерполяции через стык.
    const auto& buf = apu_.samples();
    const size_t frames = buf.size() / 2;          // число стерео-кадров
    if (frames == 0) { apu_.clearSamples(); return; }

    constexpr double SRC_RATE = (double)SnesAPU::SAMPLE_RATE; // 32000
    constexpr double DST_RATE = 44100.0;
    const double step = SRC_RATE / DST_RATE;        // вход. сэмплов на 1 выходной (~0.7256)

    auto mono = [&](long idx) -> float {
        if (idx < 0) return lastMono_;
        if (idx >= (long)frames) idx = (long)frames - 1;
        return ((float)buf[idx * 2] + (float)buf[idx * 2 + 1]) * 0.5f / 32768.0f;
    };

    audioF_.reserve(audioF_.size() + (size_t)(frames / step) + 2);
    while (resamplePos_ < (double)frames) {
        long   i    = (long)std::floor(resamplePos_);
        double frac = resamplePos_ - (double)i;
        float  a    = mono(i);
        float  b    = mono(i + 1);
        audioF_.push_back(a + (b - a) * (float)frac);  // линейная интерполяция
        resamplePos_ += step;
    }
    resamplePos_ -= (double)frames;                 // переносим остаток фазы
    lastMono_ = mono((long)frames - 1);             // запоминаем хвост для стыка

    apu_.clearSamples();
}

uint32_t* SnesConsole::getFramebuffer()
{
    return ppu_.framebuffer().data();
}

const uint32_t* SnesConsole::getFramebuffer() const
{
    return ppu_.framebuffer().data();
}

void SnesConsole::setInput(int player, uint16_t buttons)
{
    if (player >= 0 && player < 2) {
        bus_.controller[player] = buttons;
    }
}

// ─── Save State (формат "SNSS" v1) ───────────────────────────────────────────
static constexpr uint32_t SNES_SAVE_MAGIC   = 0x53534E53u;  // "SNSS"
static constexpr uint32_t SNES_SAVE_VERSION = 1u;

bool SnesConsole::saveState(std::ostream& os) const
{
    auto w8  = [&](uint8_t  v){ os.put((char)v); };
    auto w16 = [&](uint16_t v){ w8((uint8_t)(v)); w8((uint8_t)(v >> 8)); };
    auto w32 = [&](uint32_t v){ w8((uint8_t)(v)); w8((uint8_t)(v>>8)); w8((uint8_t)(v>>16)); w8((uint8_t)(v>>24)); };
    auto w64 = [&](uint64_t v){ w32((uint32_t)(v)); w32((uint32_t)(v>>32)); };

    w32(SNES_SAVE_MAGIC);
    w32(SNES_SAVE_VERSION);

    // CPU-регистры
    w16(cpu_.A);  w16(cpu_.X);  w16(cpu_.Y);
    w16(cpu_.SP); w16(cpu_.PC); w16(cpu_.D);
    w8(cpu_.PBR); w8(cpu_.DBR); w8(cpu_.P);
    w8(cpu_.E ? 1 : 0);
    w64(cpu_.totalCycles_);

    // PPU State
    SnesPPU::State ps = ppu_.getState();
    w16(ps.scanline);
    w16(ps.dot);
    os.write(reinterpret_cast<const char*>(ps.regs), sizeof(ps.regs));

    // WRAM (128 KB)
    os.write(reinterpret_cast<const char*>(bus_.wram()), (std::streamsize)SnesBus::WRAM_SIZE);

    return os.good();
}

bool SnesConsole::loadState(std::istream& is)
{
    auto r8  = [&]() -> uint8_t  { return (uint8_t)is.get(); };
    auto r16 = [&]() -> uint16_t { uint8_t lo=r8(), hi=r8(); return (uint16_t)(lo|(hi<<8)); };
    auto r32 = [&]() -> uint32_t { uint16_t lo=r16(), hi=r16(); return (uint32_t)(lo|(hi<<16)); };
    auto r64 = [&]() -> uint64_t { uint32_t lo=r32(), hi=r32(); return (uint64_t)(lo)|((uint64_t)(hi)<<32); };

    if (r32() != SNES_SAVE_MAGIC)   return false;
    if (r32() != SNES_SAVE_VERSION) return false;

    cpu_.A   = r16(); cpu_.X  = r16(); cpu_.Y  = r16();
    cpu_.SP  = r16(); cpu_.PC = r16(); cpu_.D  = r16();
    cpu_.PBR = r8();  cpu_.DBR = r8(); cpu_.P  = r8();
    cpu_.E   = (r8() != 0);
    cpu_.totalCycles_ = r64();

    SnesPPU::State ps{};
    ps.scanline = r16();
    ps.dot      = r16();
    is.read(reinterpret_cast<char*>(ps.regs), sizeof(ps.regs));
    ppu_.setState(ps);

    is.read(reinterpret_cast<char*>(bus_.wram()), (std::streamsize)SnesBus::WRAM_SIZE);

    return is.good();
}
