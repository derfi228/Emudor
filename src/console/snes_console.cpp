// snes_console.cpp — реализация SnesConsole
#include "snes_console.h"
#include "state_io.h"
#include <sstream>
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

    int cpuUnits = 0;   // «долг» CPU в половинках дота

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

        // ── CPU: исполняем по ТАКТАМ инструкций, а не «одна на 2 дота» ───────
        // Такт 65816 = 6 мастер-тактов (быстрая шина) или 8 (медленная), дот PPU
        // = 4. Считаем в половинках дота: дот = 2 единицы, такт CPU = 3 или 4.
        // Прежняя модель гнала CPU вчетверо быстрее железа, и порядок событий
        // внутри кадра переворачивался.
        cpuUnits += 2;
        while (cpuUnits > 0) {
            // IRQ — уровень (таймер $4211 или чип картриджа): висит, пока игра
            // его не снимет, и срабатывает, как только CPU разрешит прерывания.
            // WAI будит даже при запрещённых.
            if (bus_.irqLine()) {
                cpu_.waiting_ = false;
                if (!(cpu_.P & CPU65816::FLAG_I)) cpu_.irq();
            }
            if (cpu_.stopped_ || cpu_.waiting_) { cpuUnits = 0; break; }
            cpu_.clock();
            cpuUnits -= (int)bus_.cpuCycleUnits()
                      * (cpu_.pendingCycles_ > 0 ? cpu_.pendingCycles_ : 2);
            cpuUnits -= (int)bus_.takeDmaUnits();   // блочная DMA тоже ест время
        }

        // ── SuperFX/GSU: работает параллельно CPU, по 4 мастер-такта на дот ──
        bus_.runSuperFX(MASTER_PER_PPU_DOT);

        // ── SPC700: интерлив по мастер-такту (конкурентно с CPU) ─────────────
        while (spcNextTick_ <= masterClock_) {
            int c = apu_.stepOne();                              // одна инструкция SPC
            spcNextTick_ += (uint64_t)c * MASTER_PER_SPC_CYCLE;  // её длительность
            apu_.tickDsp(c);                                     // DSP-сэмплы (~32 кГц)
        }

        // ── IRQ таймера ($4200 биты 5:4) — по позиции луча ───────────────────
        // 01: на каждой строке в точке HTIME; 10: в начале строки VTIME;
        // 11: в точке (HTIME, VTIME). Цель за пределами строки (HTIME > 339)
        // на железе не срабатывает никогда. Флаг поднимается здесь, а в
        // прерывание CPU уходит по линии IRQ (см. цикл CPU выше).
        if (uint8_t mode = bus_.irqMode()) {
            const int line = dot / 341, hpos = dot % 341;
            const int ht = (int)bus_.hTarget(), vt = (int)bus_.vTarget();
            bool hit;
            if      (mode == 1) hit = ht <= 339 && hpos == ht;
            else if (mode == 2) hit = hpos == 0 && line == vt;
            else                hit = ht <= 339 && hpos == ht && line == vt;
            if (hit) bus_.raiseIrq();
        }

        // ── HDMA: раз в сканлайн, только по видимым строкам ──────────────────
        // В VBlank HDMA гонять нельзя: resetHDMA() на NMI уже выставил
        // hdmaInit_, и прогон пере-инициализировал бы канал и «съел» начало
        // таблицы → эффект съезжал по вертикали (арена Street Fighter II,
        // дождь Zelda, фон EarthBound). Видимых строк 224, в overscan — 239.
        if (dot % 341 == 0) {
            int scanline = dot / 341;
            if (scanline < ppu_.vblankStart()) bus_.runHDMA();
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

    // ─── Диагностика SuperFX: состояние GSU раз в кадр (EMUDOR_GSU_TRACE) ────
    static const bool s_gsuTrace = std::getenv("EMUDOR_GSU_TRACE") != nullptr;
    if (s_gsuTrace && bus_.hasSuperFX()) {
        const SuperFX& g = bus_.superFX();
        fprintf(stderr, "f=%d GSU %s PBR:R15=%02X:%04X SFR=%04X insn=%llu | CPU %02X:%04X P=%02X wait=%d\n",
                dbgFrames_, g.running() ? "RUN " : "stop", g.dbgPBR(), g.dbgR(15), g.dbgSFR(),
                (unsigned long long)g.dbgInstructions(), cpu_.PBR, cpu_.PC, cpu_.P, (int)cpu_.waiting_);
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

// ─── Save State (формат "SNSS" v2) ───────────────────────────────────────────
// Полное состояние: CPU, PPU (VRAM/CGRAM/OAM и все защёлки), звук (SPC700,
// DSP, 64 КБ ОЗУ, таймеры), шина (WRAM, SRAM, DMA/HDMA, IRQ, джойпад),
// SuperFX и DSP-1, общая шкала тактов. v1 хранил только CPU/PPU-регистры/WRAM
// и после загрузки картинка и звук разваливались.
static constexpr uint32_t SNES_SAVE_MAGIC   = 0x53534E53u;  // "SNSS"
static constexpr uint32_t SNES_SAVE_VERSION = 2u;

template<class S> void SnesConsole::serialize(S& s)
{
    s.expect(SNES_SAVE_MAGIC);
    s.expect(SNES_SAVE_VERSION);
    s.expect(bus_.romHash());          // состояние от другой игры не подойдёт
    cpu_.serialize(s);
    ppu_.serialize(s);
    apu_.serialize(s);
    bus_.serialize(s);
    s.io(masterClock_); s.io(spcNextTick_);
    s.io(resamplePos_); s.io(lastMono_); s.io(dbgFrames_);
}

bool SnesConsole::saveState(std::ostream& os) const
{
    StateWriter w(os);
    const_cast<SnesConsole*>(this)->serialize(w);   // запись ничего не меняет
    return w.ok();
}

// Загрузка атомарная: если файл чужой, старый или обрезан — откатываемся к
// состоянию до попытки, эмуляция не портится.
bool SnesConsole::loadState(std::istream& is)
{
    std::stringstream backup;
    StateWriter w(backup);
    serialize(w);

    StateReader r(is);
    serialize(r);
    if (!r.ok()) {
        StateReader undo(backup);
        serialize(undo);
        return false;
    }
    audioF_.clear();
    apu_.clearSamples();
    return true;
}
