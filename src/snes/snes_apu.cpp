// snes_apu.cpp — SPC700 CPU + DSP (звук SNES)
// SPC700 = Sony 8-bit CPU, ~1.024 MHz, архитектура похожа на 6502.
// Все операции с памятью идут через spcRead/spcWrite для корректного
// маппинга портов и IPL ROM.
//
// ─── АРХИТЕКТУРА ЗВУКА ────────────────────────────────────────────────────────
// Реализовано:
//   * Cycle-accurate тайминг: каждый PPU-дот добавляет бюджет тактов SPC
//     (addCycles), реальное исполнение (flush) — на доступах к портам $2140-$2143
//     и в конце кадра. SPC700 всегда «догнан» до момента взаимодействия с CPU.
//   * Полный DSP (genSample): BRR ADPCM декодер с 4 фильтрами, 8 голосов,
//     питч-аккумулятор, огибающая (attack/release), микс с per-voice VOL и MVOL.
//   * burst в IPL-фазе: надёжно проводит upload-протокол (все игры грузятся).
//
// ИЗВЕСТНОЕ ОГРАНИЧЕНИЕ: после upload звуковой драйвер игры не всегда стартует
// (SPC остаётся в IPL-ожидании) — для гарантированного запуска драйвера всех игр
// нужен потактово-точный SPC700 (точные такты на инструкцию), сверенный с
// эталоном. DSP-конвейер готов и заработает, как только драйвер начнёт писать
// регистры голосов. Игры при этом полностью рабочие визуально/по геймплею.
#include "snes_apu.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <string>

// ─── Мини-дизассемблер SPC700 (только для отладочной трассировки) ──────────────
// Возвращает "OP b1 b2  MNEMONIC". Полное декодирование операндов не требуется —
// достаточно опкода, мнемоники и сырых байт, чтобы видеть ветвления.
namespace {
const char* spcMnem(uint8_t op)
{
    switch (op) {
    case 0x00: return "NOP";
    case 0x01: case 0x11: case 0x21: case 0x31: case 0x41: case 0x51:
    case 0x61: case 0x71: case 0x81: case 0x91: case 0xA1: case 0xB1:
    case 0xC1: case 0xD1: case 0xE1: case 0xF1: return "TCALL";
    case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xA2:
    case 0xC2: case 0xE2: return "SET1";
    case 0x12: case 0x32: case 0x52: case 0x72: case 0x92: case 0xB2:
    case 0xD2: case 0xF2: return "CLR1";
    case 0x03: case 0x23: case 0x43: case 0x63: case 0x83: case 0xA3:
    case 0xC3: case 0xE3: return "BBS";
    case 0x13: case 0x33: case 0x53: case 0x73: case 0x93: case 0xB3:
    case 0xD3: case 0xF3: return "BBC";
    case 0x10: return "BPL"; case 0x30: return "BMI";
    case 0x50: return "BVC"; case 0x70: return "BVS";
    case 0x90: return "BCC"; case 0xB0: return "BCS";
    case 0xD0: return "BNE"; case 0xF0: return "BEQ";
    case 0x2F: return "BRA";
    case 0x2E: return "CBNE_dp"; case 0xDE: return "CBNE_dpX";
    case 0x6E: return "DBNZ_dp"; case 0xFE: return "DBNZ_Y";
    case 0x5F: return "JMP";  case 0x1F: return "JMP[X]";
    case 0x3F: return "CALL"; case 0x4F: return "PCALL"; case 0x6F: return "RET";
    case 0x7F: return "RETI"; case 0x0F: return "BRK";
    case 0xCF: return "MUL";  case 0x9E: return "DIV";
    case 0x20: return "CLRP"; case 0x40: return "SETP";
    case 0x60: return "CLRC"; case 0x80: return "SETC"; case 0xED: return "NOTC";
    case 0xE0: return "CLRV"; case 0x28: return "AND_A#";
    case 0xE8: return "MOV_A#"; case 0xCD: return "MOV_X#"; case 0x8D: return "MOV_Y#";
    case 0xE4: return "MOV_A_dp"; case 0xEB: return "MOV_Y_dp"; case 0xF8: return "MOV_X_dp";
    case 0xE5: return "MOV_A_abs"; case 0xE6: return "MOV_A_(X)";
    case 0xF4: return "MOV_A_dpX"; case 0xF5: return "MOV_A_absX"; case 0xF6: return "MOV_A_absY";
    case 0xF7: return "MOV_A_[dp]Y"; case 0xE7: return "MOV_A_[dpX]";
    case 0xBD: return "MOV_SP_X"; case 0x9D: return "MOV_X_SP";
    case 0x5D: return "MOV_X_A"; case 0x7D: return "MOV_A_X";
    case 0xDD: return "MOV_A_Y"; case 0xFD: return "MOV_Y_A";
    case 0x9F: return "XCN"; case 0xDA: return "MOVW_dp_YA"; case 0xBA: return "MOVW_YA_dp";
    case 0xC4: return "MOV_dp_A"; case 0xC5: return "MOV_abs_A"; case 0xC6: return "MOV_(X)_A";
    case 0xC7: return "MOV_[dpX]_A"; case 0xCB: return "MOV_dp_Y"; case 0xCC: return "MOV_abs_Y";
    case 0xD4: return "MOV_dpX_A"; case 0xD5: return "MOV_absX_A"; case 0xD6: return "MOV_absY_A";
    case 0xD7: return "MOV_[dp]Y_A"; case 0xD8: return "MOV_dp_X"; case 0xD9: return "MOV_dpY_X";
    case 0xDB: return "MOV_dpX_Y"; case 0xAF: return "MOV_(X+)_A"; case 0xBF: return "MOV_A_(X+)";
    case 0xFA: return "MOV_dp_dp"; case 0x8F: return "MOV_dp_#";
    case 0x68: return "CMP_A#"; case 0x64: return "CMP_A_dp"; case 0x65: return "CMP_A_abs";
    case 0x78: return "CMP_dp_#"; case 0x69: return "CMP_dp_dp"; case 0x79: return "CMP_(X)(Y)";
    case 0xC8: return "CMP_X#"; case 0x3E: return "CMP_X_dp"; case 0x1E: return "CMP_X_abs";
    case 0xAD: return "CMP_Y#"; case 0x7E: return "CMP_Y_dp"; case 0x5E: return "CMP_Y_abs";
    case 0x88: return "ADC_A#"; case 0xA8: return "SBC_A#";
    case 0x08: return "OR_A#"; case 0x48: return "EOR_A#";
    case 0x3D: return "INC_X"; case 0xFC: return "INC_Y"; case 0xBC: return "INC_A";
    case 0x1D: return "DEC_X"; case 0xDC: return "DEC_Y"; case 0x9C: return "DEC_A";
    case 0xAB: return "INC_dp"; case 0x8B: return "DEC_dp"; case 0xAC: return "INC_abs"; case 0x8C: return "DEC_abs";
    case 0x1C: return "ASL_A"; case 0x5C: return "LSR_A"; case 0x3C: return "ROL_A"; case 0x7C: return "ROR_A";
    case 0x0B: return "ASL_dp"; case 0x4B: return "LSR_dp"; case 0x2B: return "ROL_dp"; case 0x6B: return "ROR_dp";
    case 0x2D: return "PUSH_A"; case 0x4D: return "PUSH_X"; case 0x6D: return "PUSH_Y"; case 0x0D: return "PUSH_P";
    case 0xAE: return "POP_A"; case 0xCE: return "POP_X"; case 0xEE: return "POP_Y"; case 0x8E: return "POP_P";
    case 0xBB: return "INC_dpX"; case 0x9B: return "DEC_dpX";
    case 0xC9: return "MOV_abs_X"; case 0xE9: return "MOV_X_abs"; case 0xEC: return "MOV_Y_abs";
    case 0x7A: return "ADDW"; case 0x9A: return "SUBW"; case 0x5A: return "CMPW";
    case 0x3A: return "INCW"; case 0x1A: return "DECW";
    case 0xFF: return "STOP"; case 0xEF: return "SLEEP";
    default:   return "???";
    }
}
std::string disasmSpc(const uint8_t* ram, uint16_t pc)
{
    char buf[40];
    snprintf(buf, sizeof buf, "%02X %02X %02X %-10s",
             ram[pc], ram[(uint16_t)(pc+1)], ram[(uint16_t)(pc+2)], spcMnem(ram[pc]));
    return std::string(buf);
}
} // namespace

// ─── IPL ROM (Sony, неизменный) ───────────────────────────────────────────────
// Оригинальный 64-байтовый загрузчик SNES APU.
const uint8_t kSpcIplRom[64] = {
    0xCD,0xEF,0xBD,0xE8,0x00,0xC6,0x1D,0xD0,0xFC,0x8F,0xAA,0xF4,0x8F,0xBB,0xF5,0x78,
    0xCC,0xF4,0xD0,0xFB,0x2F,0x19,0xEB,0xF4,0xD0,0xFC,0x7E,0xF4,0xD0,0x0B,0xE4,0xF5,
    0xCB,0xF4,0xD7,0x00,0xFC,0xD0,0xF3,0xAB,0x01,0x10,0xEF,0x7E,0xF4,0x10,0xEB,0xBA,
    0xF6,0xDA,0x00,0xBA,0xF4,0xC4,0xF4,0xDD,0x5D,0xD0,0xDB,0x1F,0x00,0x00,0xC0,0xFF
};

// ─── Reset ────────────────────────────────────────────────────────────────────
void SnesAPU::reset()
{
    std::memset(portIn_,  0, sizeof(portIn_));
    std::memset(portOut_, 0, sizeof(portOut_));
    samples_.clear();
    sampleDiv_ = 0;
    dsp_.fill(0);
    for (auto& p : dspPhase_) p = 0;
    dspKonLatch_ = 0;

    spcReset();
}

void SnesAPU::spcReset()
{
    ram_.fill(0);
    // Загружаем IPL ROM в $FFC0–$FFFF
    std::memcpy(&ram_[0xFFC0], kSpcIplRom, 64);

    // Вектор сброса
    spcPC_  = (uint16_t)(ram_[0xFFFE] | (ram_[0xFFFF] << 8));
    spcA_   = 0;
    spcX_   = 0;
    spcY_   = 0;
    spcSP_  = 0xEF;
    spcPSW_ = 0x02;  // Z=1

    // ROMEN ($F1 бит7) на reset = 1: IPL ROM виден в $FFC0-$FFFF (как на железе).
    // Иначе после upload в верхнюю RAM при re-entry в IPL читается мусор.
    ram_[0x00F1] = 0x80;

    // Порты: при старте IPL отвечает $AA $BB
    portOut_[0] = 0xAA;
    portOut_[1] = 0xBB;
    portOut_[2] = 0;
    portOut_[3] = 0;

    // Таймеры
    for (int i = 0; i < 3; ++i) {
        timerPeriod_[i]  = 0;
        timerIntern_[i]  = 0;
        timerCounter_[i] = 0;
        timerDiv_[i]     = 0;
    }
    timerEnabled_ = 0;
}

// ─── Тик таймеров SPC700 ─────────────────────────────────────────────────────
void SnesAPU::tickTimers()
{
    for (int i = 0; i < 3; ++i) {
        if (!(timerEnabled_ & (1u << i))) continue;
        uint32_t div = (i < 2) ? TIMER_DIV01 : TIMER_DIV2;
        if (++timerDiv_[i] >= div) {
            timerDiv_[i] = 0;
            // Инкрементируем внутренний счётчик и проверяем период
            ++timerIntern_[i];
            uint8_t target = timerPeriod_[i];          // 0 = период 256
            if (timerIntern_[i] == target) {           // совпадение → тик выхода
                timerIntern_[i] = 0;
                timerCounter_[i] = (uint8_t)((timerCounter_[i] + 1) & 0x0F);
            }
        }
    }
}

// ─── Co-scheduler: одна инструкция SPC700 ─────────────────────────────────────
// Вызывается планировщиком (SnesConsole::runFrame) вперемежку с CPU/PPU по
// общему мастер-такту. Исполняет ровно ОДНУ инструкцию, тикает таймеры и
// возвращает её длительность в тактах SPC (для расчёта следующего события).
int SnesAPU::stepOne()
{
    int c = spcStep();
    for (int i = 0; i < c; ++i) tickTimers();
    return c;
}

// ─── DSP: генерация аудио-сэмплов ────────────────────────────────────────────
// Один стерео-сэмпл каждые 32 такта SPC (≈32 кГц).
void SnesAPU::tickDsp(int spcCycles)
{
    spcCycleAccum_ += spcCycles;
    while (spcCycleAccum_ >= AUDIO_DIV) {
        spcCycleAccum_ -= AUDIO_DIV;
        genSample();
    }
}

// ─── DSP: один стерео-сэмпл (32 кГц) — BRR ADPCM, 8 голосов, микс ──────────────
namespace {
    inline int16_t brrFilter(int s, int filter, int16_t& p0, int16_t& p1)
    {
        int out = s;
        switch (filter) {
            case 1: out = s + p0 + ((-p0) >> 4); break;
            case 2: out = s + 2*p0 + ((-3*p0) >> 5) - p1 + (p1 >> 4); break;
            case 3: out = s + 2*p0 + ((-13*p0) >> 6) - p1 + ((3*p1) >> 4); break;
            default: break;
        }
        if (out >  32767) out =  32767;
        if (out < -32768) out = -32768;
        p1 = p0; p0 = (int16_t)out;
        return (int16_t)out;
    }
}

void SnesAPU::genSample()
{
    uint8_t flg = dsp_[0x6C];
    uint8_t dir = dsp_[0x5D];

    // KON/KOFF фронты
    uint8_t kon  = dsp_[0x4C];
    uint8_t koff = dsp_[0x5C];
    for (int v = 0; v < 8; ++v) {
        uint8_t m = (uint8_t)(1 << v);
        if ((kon & m) && !(dspKonLatch_ & m)) {
            Voice& vo = voice_[v];
            uint16_t e = (uint16_t)(dir * 0x100 + dsp_[v*0x10 + 4] * 4); // SRCN
            vo.curAddr  = (uint16_t)(ram_[e] | (ram_[e+1] << 8));
            vo.loopAddr = (uint16_t)(ram_[e+2] | (ram_[e+3] << 8));
            vo.bufPos = 0; vo.bufValid = false;
            vo.prev0 = vo.prev1 = 0; vo.pitchAcc = 0;
            vo.active = true; vo.env = 0; vo.envMode = 1; // attack
        }
        if (koff & m) { if (voice_[v].active) voice_[v].envMode = 0; } // release
    }
    dspKonLatch_ = kon;

    if (flg & 0x80) { samples_.push_back(0); samples_.push_back(0); return; } // mute

    int mixL = 0, mixR = 0;
    for (int v = 0; v < 8; ++v) {
        Voice& vo = voice_[v];
        if (!vo.active) continue;
        int base = v * 0x10;

        if (!vo.bufValid) {
            uint8_t hdr = ram_[vo.curAddr];
            int range  = hdr >> 4;
            int filter = (hdr >> 2) & 3;
            for (int n = 0; n < 16; ++n) {
                uint8_t byte = ram_[(uint16_t)(vo.curAddr + 1 + (n >> 1))];
                int nib = (n & 1) ? (byte & 0x0F) : (byte >> 4);
                if (nib >= 8) nib -= 16;
                int s = (range <= 12) ? ((nib << range) >> 1) : 0;
                vo.buf[n] = brrFilter(s, filter, vo.prev0, vo.prev1);
            }
            vo.bufValid = true;
        }

        int16_t smp = vo.buf[vo.bufPos & 15];

        if (vo.envMode == 1) { vo.env += 32; if (vo.env >= 2047) { vo.env = 2047; vo.envMode = 3; } }
        else if (vo.envMode == 0) { vo.env -= 16; if (vo.env <= 0) { vo.env = 0; vo.active = false; } }

        int sval = (smp * vo.env) >> 11;
        mixL += (sval * (int8_t)dsp_[base + 0]) >> 7;
        mixR += (sval * (int8_t)dsp_[base + 1]) >> 7;

        uint16_t pitch = (uint16_t)((dsp_[base+2] | (dsp_[base+3] << 8)) & 0x3FFF);
        vo.pitchAcc += pitch;
        while (vo.pitchAcc >= 0x1000) {
            vo.pitchAcc -= 0x1000;
            if (++vo.bufPos >= 16) {
                vo.bufPos = 0;
                uint8_t hdr = ram_[vo.curAddr];
                if (hdr & 0x01) { if (hdr & 0x02) vo.curAddr = vo.loopAddr; else vo.active = false; }
                else            vo.curAddr = (uint16_t)(vo.curAddr + 9);
                vo.bufValid = false;
            }
        }
    }

    int outL = (mixL * (int8_t)dsp_[0x0C]) >> 7;
    int outR = (mixR * (int8_t)dsp_[0x1C]) >> 7;
    if (outL >  32767) outL =  32767;
    if (outL < -32768) outL = -32768;
    if (outR >  32767) outR =  32767;
    if (outR < -32768) outR = -32768;
    samples_.push_back((int16_t)outL);
    samples_.push_back((int16_t)outR);

    // ─── Диагностика DSP (EMUDOR_DSP); getenv кэширован — горячий путь ───────
    static const bool s_dspDbg = std::getenv("EMUDOR_DSP") != nullptr;
    if (s_dspDbg) {
        static long tot = 0, nz = 0; static int maxAmp = 0;
        ++tot;
        int a = outL < 0 ? -outL : outL, b = outR < 0 ? -outR : outR;
        if (a > maxAmp) maxAmp = a;
        if (b > maxAmp) maxAmp = b;
        if (outL || outR) ++nz;
        if ((tot % 96000) == 0)
            fprintf(stderr, "[DSP] tot=%ld nz=%ld maxAmp=%d spcPC=%04X kon=%02X\n",
                    tot, nz, maxAmp, spcPC_, dsp_[0x4C]);
    }
}

// ─── Порты коммуникации ──────────────────────────────────────────────────────
// Просто разделяемые массивы: CPU↔SPC синхронизирует co-scheduler (SPC уже
// исполняется вперемежку с CPU), поэтому никакого burst/flush здесь не нужно.
void SnesAPU::writePort(uint8_t port, uint8_t data)
{
    if (port < 4) portIn_[port] = data;
}

uint8_t SnesAPU::readPort(uint8_t port)
{
    if (port < 4) return portOut_[port];
    return 0;
}

// ─── Чтение/запись памяти SPC700 ─────────────────────────────────────────────
uint8_t SnesAPU::spcRead(uint16_t addr)
{
    // IPL ROM: видим только если бит CONTROL[7] = 1
    if (addr >= 0xFFC0 && (ram_[0x00F1] & 0x80)) {
        return kSpcIplRom[addr - 0xFFC0];
    }
    // Порты SNES→SPC ($F4–$F7)
    if (addr >= 0x00F4 && addr <= 0x00F7) {
        return portIn_[addr - 0x00F4];
    }
    // Счётчики таймеров $FD–$FF: возвращаем 4-бит значение и сбрасываем
    if (addr == 0x00FD) { uint8_t v = timerCounter_[0] & 0x0F; timerCounter_[0] = 0; return v; }
    if (addr == 0x00FE) { uint8_t v = timerCounter_[1] & 0x0F; timerCounter_[1] = 0; return v; }
    if (addr == 0x00FF) { uint8_t v = timerCounter_[2] & 0x0F; timerCounter_[2] = 0; return v; }
    // DSP-регистр читается через $F3
    if (addr == 0x00F3) {
        uint8_t idx = (uint8_t)(ram_[0x00F2] & 0x7F);
        return dsp_[idx];
    }
    return ram_[addr];
}

void SnesAPU::spcWrite(uint16_t addr, uint8_t data)
{
    ram_[addr] = data;

    // Порты SPC→SNES ($F4–$F7)
    if (addr >= 0x00F4 && addr <= 0x00F7) {
        portOut_[addr - 0x00F4] = data;
        return;
    }

    // CONTROL регистр $F1: управление таймерами и IPL ROM
    if (addr == 0x00F1) {
        // Биты 0-2: включить/выключить таймеры
        for (int i = 0; i < 3; ++i) {
            bool wasEnabled = (timerEnabled_ & (1u << i)) != 0;
            bool nowEnabled = (data & (1u << i)) != 0;
            if (!wasEnabled && nowEnabled) {
                // При включении — полный сброс
                timerDiv_[i]     = 0;
                timerIntern_[i]  = 0;
                timerCounter_[i] = 0;
            }
        }
        timerEnabled_ = data & 0x07;
        // Бит 4: сброс portIn_[0/1]; бит 5: сброс portIn_[2/3]
        if (data & 0x10) { portIn_[0] = 0; portIn_[1] = 0; }
        if (data & 0x20) { portIn_[2] = 0; portIn_[3] = 0; }
        return;
    }

    // Периоды таймеров $FA–$FC
    if (addr == 0x00FA) { timerPeriod_[0] = data; return; }
    if (addr == 0x00FB) { timerPeriod_[1] = data; return; }
    if (addr == 0x00FC) { timerPeriod_[2] = data; return; }

    // DSP-регистры: $F3 пишет в dsp_[ram_[$F2] & 0x7F].
    // ВАЖНО: фронт KON детектит genSample() (сравнивая dsp_[$4C] с dspKonLatch_).
    // Здесь dspKonLatch_ НЕ трогаем — иначе фронт не сработает и голос не стартует.
    if (addr == 0x00F3) {
        uint8_t idx = (uint8_t)(ram_[0x00F2] & 0x7F);
        dsp_[idx] = data;
        return;
    }
}

// ─── Вспомогательные методы ───────────────────────────────────────────────────
uint8_t SnesAPU::spcFetch()
{
    return spcRead(spcPC_++);
}

void SnesAPU::spcPush(uint8_t v)
{
    ram_[0x0100 + spcSP_--] = v;
}

uint8_t SnesAPU::spcPop()
{
    return ram_[0x0100 + ++spcSP_];
}

void SnesAPU::setNZ(uint8_t v)
{
    spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N | FL_Z))
        | (v == 0    ? FL_Z : 0)
        | (v & 0x80  ? FL_N : 0));
}

// ─── SPC700: одна инструкция ──────────────────────────────────────────────────
// Реализованы наиболее частые опкоды (~ достаточно для IPL ROM).
// Полная реализация выходит за рамки данной фазы.
int SnesAPU::spcStep()
{
    // ─── Однократная трассировка пути движка SPC700 от $0500 (EMUDOR_SPCTRACE) ─
    // getenv кэшируется (один раз), чтобы не бить по производительности.
    static const bool s_spcTrace = std::getenv("EMUDOR_SPCTRACE") != nullptr;
    if (s_spcTrace) {
        static bool active = false;
        static int  limit  = 2000;
        if (spcPC_ == 0x0500) active = true;
        if (active && limit > 0) {
            --limit;
            fprintf(stderr,
                "SPC %04X  %s A=%02X X=%02X Y=%02X SP=%02X PSW=%02X  r04=%02X r05=%02X  "
                "IN=%02X%02X%02X%02X OUT=%02X%02X%02X%02X\n",
                spcPC_, disasmSpc(ram_.data(), spcPC_).c_str(),
                spcA_, spcX_, spcY_, spcSP_, spcPSW_,
                ram_[0x04], ram_[0x05],
                portIn_[0], portIn_[1], portIn_[2], portIn_[3],
                portOut_[0], portOut_[1], portOut_[2], portOut_[3]);
            if (limit == 0) fprintf(stderr, "SPC TRACE LIMIT\n");
        }
    }

    uint8_t op = spcFetch();
    uint8_t dp = (spcPSW_ & FL_P) ? 0x01 : 0x00;  // Direct Page: $00xx или $01xx

    auto dpAddr = [&](uint8_t d) -> uint16_t { return (uint16_t)((dp << 8) | d); };
    auto absAddr = [&]() -> uint16_t {
        uint8_t lo = spcFetch(), hi = spcFetch();
        return (uint16_t)(lo | (hi << 8));
    };

    // ── ADC/SBC: общие хелперы с ПОЛНЫМ набором флагов (N V H Z C) ────────────
    // H (half-carry) — перенос/заём между битом 3 и 4. SBC реализуем как
    // сложение с дополнением (~b), что даёт корректные C/V/H единообразно.
    auto adc8 = [&](uint8_t a, uint8_t b) -> uint8_t {
        uint8_t cin = (spcPSW_ & FL_C) ? 1 : 0;
        uint16_t r = (uint16_t)(a + b + cin);
        uint8_t  res = (uint8_t)r;
        spcPSW_ &= (uint8_t)~(FL_N | FL_V | FL_H | FL_Z | FL_C);
        if (r > 0xFF)                       spcPSW_ |= FL_C;
        if (res == 0)                       spcPSW_ |= FL_Z;
        if (res & 0x80)                     spcPSW_ |= FL_N;
        if ((a ^ b ^ res) & 0x10)           spcPSW_ |= FL_H;
        if ((a ^ res) & (b ^ res) & 0x80)   spcPSW_ |= FL_V;
        return res;
    };
    auto sbc8 = [&](uint8_t a, uint8_t b) -> uint8_t {
        uint8_t  nb  = (uint8_t)~b;
        uint8_t  cin = (spcPSW_ & FL_C) ? 1 : 0;
        uint16_t r   = (uint16_t)(a + nb + cin);
        uint8_t  res = (uint8_t)r;
        spcPSW_ &= (uint8_t)~(FL_N | FL_V | FL_H | FL_Z | FL_C);
        if (r > 0xFF)                       spcPSW_ |= FL_C;   // нет заёма
        if (res == 0)                       spcPSW_ |= FL_Z;
        if (res & 0x80)                     spcPSW_ |= FL_N;
        if ((a ^ nb ^ res) & 0x10)          spcPSW_ |= FL_H;
        if ((a ^ res) & (nb ^ res) & 0x80)  spcPSW_ |= FL_V;
        return res;
    };
    // CMP: a - b, влияет только на N Z C (без V, без H)
    auto cmp8 = [&](uint8_t a, uint8_t b) {
        uint8_t r = (uint8_t)(a - b);
        spcPSW_ &= (uint8_t)~(FL_N | FL_Z | FL_C);
        if (a >= b)   spcPSW_ |= FL_C;
        if (r == 0)   spcPSW_ |= FL_Z;
        if (r & 0x80) spcPSW_ |= FL_N;
    };
    // ── Хелперы адресации (читают операнд из потока кода) ────────────────────
    auto aDpX  = [&]() -> uint16_t { return dpAddr((uint8_t)(spcFetch() + spcX_)); };
    auto aAbsX = [&]() -> uint16_t { return (uint16_t)(absAddr() + spcX_); };
    auto aAbsY = [&]() -> uint16_t { return (uint16_t)(absAddr() + spcY_); };
    auto aIndX = [&]() -> uint16_t {          // [dp+X]
        uint8_t d = (uint8_t)(spcFetch() + spcX_);
        return (uint16_t)(spcRead(dpAddr(d)) | (spcRead(dpAddr((uint8_t)(d+1))) << 8));
    };
    auto aIndY = [&]() -> uint16_t {          // [dp]+Y
        uint8_t d = spcFetch();
        uint16_t base = (uint16_t)(spcRead(dpAddr(d)) | (spcRead(dpAddr((uint8_t)(d+1))) << 8));
        return (uint16_t)(base + spcY_);
    };

    switch (op) {
    // ── NOP ──────────────────────────────────────────────────────────────────
    case 0x00: break;

    // ── MOV A, #imm ──────────────────────────────────────────────────────────
    case 0xE8: spcA_ = spcFetch(); setNZ(spcA_); break;

    // ── MOV X, #imm ──────────────────────────────────────────────────────────
    case 0xCD: spcX_ = spcFetch(); setNZ(spcX_); break;

    // ── MOV Y, #imm ──────────────────────────────────────────────────────────
    case 0x8D: spcY_ = spcFetch(); setNZ(spcY_); break;

    // ── MOV A, dp ─────────────────────────────────────────────────────────────
    case 0xE4: { uint8_t d = spcFetch(); spcA_ = spcRead(dpAddr(d)); setNZ(spcA_); break; }

    // ── MOV A, dp+X ──────────────────────────────────────────────────────────
    case 0xF4: { uint8_t d = spcFetch(); spcA_ = spcRead(dpAddr((uint8_t)(d+spcX_))); setNZ(spcA_); break; }

    // ── MOV A, !abs ──────────────────────────────────────────────────────────
    case 0xE5: { uint16_t a = absAddr(); spcA_ = spcRead(a); setNZ(spcA_); break; }

    // ── MOV A, X ──────────────────────────────────────────────────────────────
    case 0x7D: spcA_ = spcX_; setNZ(spcA_); break;

    // ── MOV A, Y ──────────────────────────────────────────────────────────────
    case 0xDD: spcA_ = spcY_; setNZ(spcA_); break;

    // ── MOV X, A ──────────────────────────────────────────────────────────────
    case 0x5D: spcX_ = spcA_; setNZ(spcX_); break;

    // ── MOV Y, A ──────────────────────────────────────────────────────────────
    case 0xFD: spcY_ = spcA_; setNZ(spcY_); break;

    // ── MOV X, dp ─────────────────────────────────────────────────────────────
    case 0xF8: { uint8_t d = spcFetch(); spcX_ = spcRead(dpAddr(d)); setNZ(spcX_); break; }

    // ── MOV Y, dp ─────────────────────────────────────────────────────────────
    case 0xEB: { uint8_t d = spcFetch(); spcY_ = spcRead(dpAddr(d)); setNZ(spcY_); break; }

    // ── MOV dp, A ────────────────────────────────────────────────────────────
    case 0xC4: { uint8_t d = spcFetch(); spcWrite(dpAddr(d), spcA_); break; }

    // ── MOV dp+X, A ──────────────────────────────────────────────────────────
    case 0xD4: { uint8_t d = spcFetch(); spcWrite(dpAddr((uint8_t)(d+spcX_)), spcA_); break; }

    // ── MOV !abs, A ──────────────────────────────────────────────────────────
    case 0xC5: { uint16_t a = absAddr(); spcWrite(a, spcA_); break; }

    // ── MOV dp, #imm ─────────────────────────────────────────────────────────
    case 0x8F: { uint8_t imm = spcFetch(); uint8_t d = spcFetch(); spcWrite(dpAddr(d), imm); break; }

    // ── MOV (X), A ───────────────────────────────────────────────────────────
    case 0xC6: spcWrite(dpAddr(spcX_), spcA_); break;

    // ── MOV A, (X)+ (post-increment) ─────────────────────────────────────────
    case 0xBF: { spcA_ = spcRead(dpAddr(spcX_++)); setNZ(spcA_); break; }

    // ── MOV (X)+, A ──────────────────────────────────────────────────────────
    case 0xAF: spcWrite(dpAddr(spcX_++), spcA_); break;

    // ── MOV dp, dp ───────────────────────────────────────────────────────────
    case 0xFA: { uint8_t s = spcFetch(), d = spcFetch();
                 spcWrite(dpAddr(d), spcRead(dpAddr(s))); break; }

    // ── PUSH A ───────────────────────────────────────────────────────────────
    case 0x2D: spcPush(spcA_); break;
    case 0x4D: spcPush(spcX_); break;
    case 0x6D: spcPush(spcY_); break;
    case 0x0D: spcPush(spcPSW_); break;

    // ── POP A ────────────────────────────────────────────────────────────────
    case 0xAE: spcA_   = spcPop(); break;
    case 0xCE: spcX_   = spcPop(); break;
    case 0xEE: spcY_   = spcPop(); break;
    case 0x8E: spcPSW_ = spcPop(); break;

    // ── ADC A, #imm ($88) ────────────────────────────────────────────────────
    case 0x88: { uint8_t imm = spcFetch(); spcA_ = adc8(spcA_, imm); break; }
    // ── ADC A, dp+X ($94) ────────────────────────────────────────────────────
    case 0x94: { uint8_t d = spcFetch(); spcA_ = adc8(spcA_, spcRead(dpAddr((uint8_t)(d+spcX_)))); break; }
    // ── ADC A, abs ($85) ─────────────────────────────────────────────────────
    case 0x85: { uint16_t a = absAddr(); spcA_ = adc8(spcA_, spcRead(a)); break; }
    // ── ADC A, abs+X ($95) ───────────────────────────────────────────────────
    case 0x95: { uint16_t a = (uint16_t)(absAddr()+spcX_); spcA_ = adc8(spcA_, spcRead(a)); break; }
    // ── ADC A, abs+Y ($96) ───────────────────────────────────────────────────
    case 0x96: { uint16_t a = (uint16_t)(absAddr()+spcY_); spcA_ = adc8(spcA_, spcRead(a)); break; }
    // ── ADC A, (X) ($86) ─────────────────────────────────────────────────────
    case 0x86: { spcA_ = adc8(spcA_, spcRead(dpAddr(spcX_))); break; }
    // ── ADC A, [dp+X] ($87) ──────────────────────────────────────────────────
    case 0x87: { uint8_t d=(uint8_t)(spcFetch()+spcX_);
        uint16_t a=(uint16_t)(spcRead(dpAddr(d))|(spcRead(dpAddr((uint8_t)(d+1)))<<8));
        spcA_ = adc8(spcA_, spcRead(a)); break; }
    // ── ADC A, [dp]+Y ($97) ──────────────────────────────────────────────────
    case 0x97: { uint8_t d=spcFetch();
        uint16_t a=(uint16_t)((spcRead(dpAddr(d))|(spcRead(dpAddr((uint8_t)(d+1)))<<8))+spcY_);
        spcA_ = adc8(spcA_, spcRead(a)); break; }
    // ── ADC (X),(Y) ($99) ────────────────────────────────────────────────────
    case 0x99: { uint16_t ax=dpAddr(spcX_); uint8_t r=adc8(spcRead(ax), spcRead(dpAddr(spcY_)));
        spcWrite(ax, r); break; }

    // ── SBC A, #imm ($A8) ────────────────────────────────────────────────────
    case 0xA8: { uint8_t imm = spcFetch(); spcA_ = sbc8(spcA_, imm); break; }
    // ── SBC A, dp+X ($B4) ────────────────────────────────────────────────────
    case 0xB4: { uint8_t d = spcFetch(); spcA_ = sbc8(spcA_, spcRead(dpAddr((uint8_t)(d+spcX_)))); break; }
    // ── SBC A, abs ($A5) ─────────────────────────────────────────────────────
    case 0xA5: { uint16_t a = absAddr(); spcA_ = sbc8(spcA_, spcRead(a)); break; }
    // ── SBC A, abs+X ($B5) ───────────────────────────────────────────────────
    case 0xB5: { uint16_t a = (uint16_t)(absAddr()+spcX_); spcA_ = sbc8(spcA_, spcRead(a)); break; }
    // ── SBC A, abs+Y ($B6) ───────────────────────────────────────────────────
    case 0xB6: { uint16_t a = (uint16_t)(absAddr()+spcY_); spcA_ = sbc8(spcA_, spcRead(a)); break; }
    // ── SBC A, (X) ($A6) ─────────────────────────────────────────────────────
    case 0xA6: { spcA_ = sbc8(spcA_, spcRead(dpAddr(spcX_))); break; }
    // ── SBC A, [dp+X] ($A7) ──────────────────────────────────────────────────
    case 0xA7: { uint8_t d=(uint8_t)(spcFetch()+spcX_);
        uint16_t a=(uint16_t)(spcRead(dpAddr(d))|(spcRead(dpAddr((uint8_t)(d+1)))<<8));
        spcA_ = sbc8(spcA_, spcRead(a)); break; }
    // ── SBC A, [dp]+Y ($B7) ──────────────────────────────────────────────────
    case 0xB7: { uint8_t d=spcFetch();
        uint16_t a=(uint16_t)((spcRead(dpAddr(d))|(spcRead(dpAddr((uint8_t)(d+1)))<<8))+spcY_);
        spcA_ = sbc8(spcA_, spcRead(a)); break; }
    // ── SBC (X),(Y) ($B9) ────────────────────────────────────────────────────
    case 0xB9: { uint16_t ax=dpAddr(spcX_); uint8_t r=sbc8(spcRead(ax), spcRead(dpAddr(spcY_)));
        spcWrite(ax, r); break; }
    // ── SBC dp, #imm ($B8) ───────────────────────────────────────────────────
    case 0xB8: { uint8_t imm=spcFetch(); uint8_t d=spcFetch(); uint16_t a=dpAddr(d);
        spcWrite(a, sbc8(spcRead(a), imm)); break; }

    // ── CMP A, #imm ──────────────────────────────────────────────────────────
    case 0x68: {
        uint8_t imm = spcFetch();
        uint8_t r = (uint8_t)(spcA_ - imm);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcA_ >= imm ? FL_C : 0)
            | (r == 0 ? FL_Z : 0)
            | (r & 0x80 ? FL_N : 0));
        break;
    }

    // ── CMP X, #imm ──────────────────────────────────────────────────────────
    case 0xC8: {
        uint8_t imm = spcFetch();
        uint8_t r = (uint8_t)(spcX_ - imm);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcX_ >= imm ? FL_C : 0)
            | (r == 0 ? FL_Z : 0)
            | (r & 0x80 ? FL_N : 0));
        break;
    }

    // ── CMP A, dp ────────────────────────────────────────────────────────────
    case 0x64: {
        uint8_t d = spcFetch();
        uint8_t val = spcRead(dpAddr(d));
        uint8_t r = (uint8_t)(spcA_ - val);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcA_ >= val ? FL_C : 0)
            | (r == 0 ? FL_Z : 0)
            | (r & 0x80 ? FL_N : 0));
        break;
    }

    // ── AND A, #imm ──────────────────────────────────────────────────────────
    case 0x28: spcA_ &= spcFetch(); setNZ(spcA_); break;

    // ── OR A, #imm ───────────────────────────────────────────────────────────
    case 0x08: spcA_ |= spcFetch(); setNZ(spcA_); break;

    // ── EOR A, #imm ──────────────────────────────────────────────────────────
    case 0x48: spcA_ ^= spcFetch(); setNZ(spcA_); break;

    // ── INC A ────────────────────────────────────────────────────────────────
    case 0xBC: ++spcA_; setNZ(spcA_); break;
    case 0x3D: ++spcX_; setNZ(spcX_); break;
    case 0xFC: ++spcY_; setNZ(spcY_); break;

    // ── DEC A ────────────────────────────────────────────────────────────────
    case 0x9C: --spcA_; setNZ(spcA_); break;
    case 0x1D: --spcX_; setNZ(spcX_); break;
    case 0xDC: --spcY_; setNZ(spcY_); break;

    // ── INC dp ───────────────────────────────────────────────────────────────
    case 0xAB: {
        uint8_t d = spcFetch(); uint16_t a = dpAddr(d);
        uint8_t v = (uint8_t)(spcRead(a) + 1); spcWrite(a, v); setNZ(v); break;
    }
    // ── INC dp+X ($BB) ─────────────────────────────────────────────────────
    case 0xBB: {
        uint16_t a = dpAddr((uint8_t)(spcFetch() + spcX_));
        uint8_t v = (uint8_t)(spcRead(a) + 1); spcWrite(a, v); setNZ(v); break;
    }
    // ── INC abs ($AC) ──────────────────────────────────────────────────────
    case 0xAC: {
        uint16_t a = absAddr();
        uint8_t v = (uint8_t)(spcRead(a) + 1); spcWrite(a, v); setNZ(v); break;
    }
    // ── DEC dp ───────────────────────────────────────────────────────────────
    case 0x8B: {
        uint8_t d = spcFetch(); uint16_t a = dpAddr(d);
        uint8_t v = (uint8_t)(spcRead(a) - 1); spcWrite(a, v); setNZ(v); break;
    }
    // ── DEC dp+X ($9B) ─────────────────────────────────────────────────────
    case 0x9B: {
        uint16_t a = dpAddr((uint8_t)(spcFetch() + spcX_));
        uint8_t v = (uint8_t)(spcRead(a) - 1); spcWrite(a, v); setNZ(v); break;
    }
    // ── DEC abs ($8C) ──────────────────────────────────────────────────────
    case 0x8C: {
        uint16_t a = absAddr();
        uint8_t v = (uint8_t)(spcRead(a) - 1); spcWrite(a, v); setNZ(v); break;
    }

    // ── ASL A ────────────────────────────────────────────────────────────────
    case 0x1C: {
        spcPSW_ = (uint8_t)((spcPSW_ & ~FL_C) | (spcA_ >> 7));
        spcA_ <<= 1; setNZ(spcA_); break;
    }
    // ── LSR A ────────────────────────────────────────────────────────────────
    case 0x5C: {
        spcPSW_ = (uint8_t)((spcPSW_ & ~FL_C) | (spcA_ & FL_C));
        spcA_ >>= 1; setNZ(spcA_); break;
    }
    // ── ROL A ────────────────────────────────────────────────────────────────
    case 0x3C: {
        uint8_t c = spcPSW_ & FL_C;
        spcPSW_ = (uint8_t)((spcPSW_ & ~FL_C) | (spcA_ >> 7));
        spcA_ = (uint8_t)((spcA_ << 1) | c); setNZ(spcA_); break;
    }
    // ── ROR A ────────────────────────────────────────────────────────────────
    case 0x7C: {
        uint8_t c = spcPSW_ & FL_C;
        spcPSW_ = (uint8_t)((spcPSW_ & ~FL_C) | (spcA_ & FL_C));
        spcA_ = (uint8_t)((spcA_ >> 1) | (c << 7)); setNZ(spcA_); break;
    }

    // ── BRA (always branch) ────────────────────────────────────────────────
    case 0x2F: {
        int8_t off = (int8_t)spcFetch();
        spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }

    // ── BEQ ──────────────────────────────────────────────────────────────────
    case 0xF0: {
        int8_t off = (int8_t)spcFetch();
        if (spcPSW_ & FL_Z) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }
    // ── BNE ──────────────────────────────────────────────────────────────────
    case 0xD0: {
        int8_t off = (int8_t)spcFetch();
        if (!(spcPSW_ & FL_Z)) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }
    // ── BCC ──────────────────────────────────────────────────────────────────
    case 0x90: {
        int8_t off = (int8_t)spcFetch();
        if (!(spcPSW_ & FL_C)) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }
    // ── BCS ──────────────────────────────────────────────────────────────────
    case 0xB0: {
        int8_t off = (int8_t)spcFetch();
        if (spcPSW_ & FL_C) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }
    // ── BPL ──────────────────────────────────────────────────────────────────
    case 0x10: {
        int8_t off = (int8_t)spcFetch();
        if (!(spcPSW_ & FL_N)) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }
    // ── BMI ──────────────────────────────────────────────────────────────────
    case 0x30: {
        int8_t off = (int8_t)spcFetch();
        if (spcPSW_ & FL_N) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }

    // ── CBNE dp, rel ─────────────────────────────────────────────────────────
    case 0x2E: {
        uint8_t d = spcFetch(); int8_t off = (int8_t)spcFetch();
        if (spcA_ != spcRead(dpAddr(d))) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }
    // ── DBNZ Y, rel ──────────────────────────────────────────────────────────
    case 0xFE: {
        int8_t off = (int8_t)spcFetch();
        --spcY_;
        if (spcY_ != 0) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }

    // ── JMP !abs ─────────────────────────────────────────────────────────────
    case 0x5F: {
        uint16_t a = absAddr();
        spcPC_ = a;
        break;
    }

    // ── CALL !abs ────────────────────────────────────────────────────────────
    case 0x3F: {
        uint16_t a = absAddr();
        spcPush((uint8_t)(spcPC_ >> 8));
        spcPush((uint8_t)(spcPC_));
        spcPC_ = a;
        break;
    }
    // ── RET ──────────────────────────────────────────────────────────────────
    case 0x6F: {
        uint8_t lo = spcPop(), hi = spcPop();
        spcPC_ = (uint16_t)(lo | (hi << 8));
        break;
    }

    // ── MOV YA, dp (слово) ───────────────────────────────────────────────────
    case 0xBA: {
        uint8_t d = spcFetch();
        spcA_ = spcRead(dpAddr(d));
        spcY_ = spcRead(dpAddr((uint8_t)(d + 1)));
        setNZ(spcA_);
        break;
    }
    // ── MOV dp, YA ───────────────────────────────────────────────────────────
    case 0xDA: {
        uint8_t d = spcFetch();
        spcWrite(dpAddr(d), spcA_);
        spcWrite(dpAddr((uint8_t)(d + 1)), spcY_);
        break;
    }

    // ── CLRP (clear P flag) ──────────────────────────────────────────────────
    case 0x20: spcPSW_ &= (uint8_t)~FL_P; break;
    // ── SETP ─────────────────────────────────────────────────────────────────
    case 0x40: spcPSW_ |= FL_P; break;
    // ── CLRC ─────────────────────────────────────────────────────────────────
    case 0x60: spcPSW_ &= (uint8_t)~FL_C; break;
    // ── SETC ─────────────────────────────────────────────────────────────────
    case 0x80: spcPSW_ |= FL_C; break;
    // ── CLRV ─────────────────────────────────────────────────────────────────
    case 0xE0: spcPSW_ &= (uint8_t)~(FL_V | FL_H); break;
    // ── EI / DI ──────────────────────────────────────────────────────────────
    case 0xA0: spcPSW_ |= FL_I;  break;
    case 0xC0: spcPSW_ &= (uint8_t)~FL_I; break;

    // ── SET1/CLR1 dp.bit (0x12, 0x32, 0x52, 0x72, 0x92, 0xB2, 0xD2, 0xF2)
    // CLR1: 0x12, 0x32, 0x52, 0x72, 0x92, 0xB2, 0xD2, 0xF2
    // SET1: 0x02, 0x22, 0x42, 0x62, 0x82, 0xA2, 0xC2, 0xE2
    case 0x02: case 0x22: case 0x42: case 0x62:
    case 0x82: case 0xA2: case 0xC2: case 0xE2: {
        uint8_t bit = (op >> 5) & 7;
        uint8_t d = spcFetch();
        uint16_t a = dpAddr(d);
        spcWrite(a, (uint8_t)(spcRead(a) | (1u << bit)));
        break;
    }
    case 0x12: case 0x32: case 0x52: case 0x72:
    case 0x92: case 0xB2: case 0xD2: case 0xF2: {
        uint8_t bit = (op >> 5) & 7;
        uint8_t d = spcFetch();
        uint16_t a = dpAddr(d);
        spcWrite(a, (uint8_t)(spcRead(a) & ~(1u << bit)));
        break;
    }

    // ── BBS/BBC dp.bit, rel ───────────────────────────────────────────────────
    // BBS: 0x03,0x23,0x43,0x63,0x83,0xA3,0xC3,0xE3
    // BBC: 0x13,0x33,0x53,0x73,0x93,0xB3,0xD3,0xF3
    case 0x03: case 0x23: case 0x43: case 0x63:
    case 0x83: case 0xA3: case 0xC3: case 0xE3: {
        uint8_t bit = (op >> 5) & 7;
        uint8_t d = spcFetch(); int8_t off = (int8_t)spcFetch();
        if (spcRead(dpAddr(d)) & (1u << bit)) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }
    case 0x13: case 0x33: case 0x53: case 0x73:
    case 0x93: case 0xB3: case 0xD3: case 0xF3: {
        uint8_t bit = (op >> 5) & 7;
        uint8_t d = spcFetch(); int8_t off = (int8_t)spcFetch();
        if (!(spcRead(dpAddr(d)) & (1u << bit))) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }

    // ── TCALL n (вектор по адресу $FFxx) ─────────────────────────────────────
    case 0x01: case 0x11: case 0x21: case 0x31:
    case 0x41: case 0x51: case 0x61: case 0x71:
    case 0x81: case 0x91: case 0xA1: case 0xB1:
    case 0xC1: case 0xD1: case 0xE1: case 0xF1: {
        uint8_t n = (op >> 4) & 0x0F;
        uint16_t vec = (uint16_t)(0xFFDE - n * 2);
        spcPush((uint8_t)(spcPC_ >> 8));
        spcPush((uint8_t)(spcPC_));
        spcPC_ = (uint16_t)(spcRead(vec) | (spcRead((uint16_t)(vec+1)) << 8));
        break;
    }

    // ── MOV A, (X) ───────────────────────────────────────────────────────────
    case 0xE6: spcA_ = spcRead(dpAddr(spcX_)); setNZ(spcA_); break;

    // ── MOV SP, X ($BD) ──────────────────────────────────────────────────────
    case 0xBD: spcSP_ = spcX_; break;

    // ── MOV X, SP ($9D) ──────────────────────────────────────────────────────
    case 0x9D: spcX_ = spcSP_; setNZ(spcX_); break;

    // ── MOV dp, Y ($CB) ──────────────────────────────────────────────────────
    case 0xCB: { uint8_t d = spcFetch(); spcWrite(dpAddr(d), spcY_); break; }

    // ── MOV dp, X ($D8) ──────────────────────────────────────────────────────
    case 0xD8: { uint8_t d = spcFetch(); spcWrite(dpAddr(d), spcX_); break; }

    // ── MOV Y, dp+X ($FB) ────────────────────────────────────────────────────
    case 0xFB: { uint8_t d = spcFetch(); spcY_ = spcRead(dpAddr((uint8_t)(d+spcX_))); setNZ(spcY_); break; }

    // ── MOV [dp]+Y, A ($D7) — косвенная запись с Y-индексом ─────────────────
    case 0xD7: {
        uint8_t  d   = spcFetch();
        uint16_t ptr = (uint16_t)(spcRead(dpAddr(d)) | (spcRead(dpAddr((uint8_t)(d+1))) << 8));
        spcWrite((uint16_t)(ptr + spcY_), spcA_);
        break;
    }

    // ── MOV A, [dp]+Y ($F7) — косвенное чтение с Y-индексом ─────────────────
    case 0xF7: {
        uint8_t  d   = spcFetch();
        uint16_t ptr = (uint16_t)(spcRead(dpAddr(d)) | (spcRead(dpAddr((uint8_t)(d+1))) << 8));
        spcA_ = spcRead((uint16_t)(ptr + spcY_));
        setNZ(spcA_);
        break;
    }

    // ── MOV A, [dp+X] ($E7) — прединдексная косвенная адресация ─────────────
    case 0xE7: {
        uint8_t  d   = spcFetch();
        uint16_t ptr = (uint16_t)(spcRead(dpAddr((uint8_t)(d+spcX_)))
                     | (spcRead(dpAddr((uint8_t)(d+spcX_+1))) << 8));
        spcA_ = spcRead(ptr);
        setNZ(spcA_);
        break;
    }

    // ── CMP Y, dp ($7E) ──────────────────────────────────────────────────────
    case 0x7E: {
        uint8_t d = spcFetch();
        uint8_t val = spcRead(dpAddr(d));
        uint8_t r = (uint8_t)(spcY_ - val);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcY_ >= val ? FL_C : 0)
            | (r == 0 ? FL_Z : 0)
            | (r & 0x80 ? FL_N : 0));
        break;
    }

    // ── CMP dp, #imm ($78) — operand order: imm, dp ──────────────────────────
    case 0x78: {
        uint8_t imm = spcFetch(); uint8_t d = spcFetch();
        uint8_t val = spcRead(dpAddr(d));
        uint8_t r = (uint8_t)(val - imm);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (val >= imm ? FL_C : 0)
            | (r == 0 ? FL_Z : 0)
            | (r & 0x80 ? FL_N : 0));
        break;
    }

    // ── CMP Y, #imm ($AD) ────────────────────────────────────────────────────
    case 0xAD: {
        uint8_t imm = spcFetch();
        uint8_t r = (uint8_t)(spcY_ - imm);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcY_ >= imm ? FL_C : 0)
            | (r == 0 ? FL_Z : 0)
            | (r & 0x80 ? FL_N : 0));
        break;
    }

    // ── ADC dp, #imm ($98) ───────────────────────────────────────────────────
    case 0x98: {
        uint8_t imm = spcFetch(); uint8_t d = spcFetch();
        uint16_t a = dpAddr(d);
        spcWrite(a, adc8(spcRead(a), imm));
        break;
    }

    // ── AND dp, #imm ($38) ───────────────────────────────────────────────────
    case 0x38: {
        uint8_t imm = spcFetch(); uint8_t d = spcFetch();
        uint16_t a = dpAddr(d);
        uint8_t v = (uint8_t)(spcRead(a) & imm);
        spcWrite(a, v); setNZ(v);
        break;
    }

    // ── OR dp, #imm ($18) ────────────────────────────────────────────────────
    case 0x18: {
        uint8_t imm = spcFetch(); uint8_t d = spcFetch();
        uint16_t a = dpAddr(d);
        uint8_t v = (uint8_t)(spcRead(a) | imm);
        spcWrite(a, v); setNZ(v);
        break;
    }

    // ── EOR dp, #imm ($58) ───────────────────────────────────────────────────
    case 0x58: {
        uint8_t imm = spcFetch(); uint8_t d = spcFetch();
        uint16_t a = dpAddr(d);
        uint8_t v = (uint8_t)(spcRead(a) ^ imm);
        spcWrite(a, v); setNZ(v);
        break;
    }

    // ── JMP [abs+X] ($1F) — абсолютная косвенная адресация с X ──────────────
    case 0x1F: {
        uint16_t base = absAddr();
        uint16_t ea   = (uint16_t)(base + spcX_);
        spcPC_ = (uint16_t)(spcRead(ea) | (spcRead((uint16_t)(ea+1)) << 8));
        break;
    }

    // ── XCN A ($9F) — swap nibbles: A = (A>>4) | (A<<4) ─────────────────────
    case 0x9F: {
        spcA_ = (uint8_t)((spcA_ >> 4) | (spcA_ << 4));
        setNZ(spcA_);
        break;
    }

    // ── MUL YA ($CF): YA = Y * A (16-bit result) ─────────────────────────────
    case 0xCF: {
        uint16_t r = (uint16_t)(spcY_ * spcA_);
        spcA_ = (uint8_t)(r & 0xFF);
        spcY_ = (uint8_t)(r >> 8);
        setNZ(spcY_);
        break;
    }

    // ── DIV YA, X ($9E) ──────────────────────────────────────────────────────
    case 0x9E: {
        if (spcX_ == 0) {
            spcA_ = 0xFF; spcY_ = 0xFF;  // division by zero
        } else {
            uint16_t ya = (uint16_t)((spcY_ << 8) | spcA_);
            spcA_ = (uint8_t)(ya / spcX_);
            spcY_ = (uint8_t)(ya % spcX_);
        }
        setNZ(spcA_);
        break;
    }

    // ── SLEEP ($EF) / STOP ($FF) — заглушка (NOP) ────────────────────────────
    case 0xEF:
    case 0xFF:
        break;

    // ── ADDW YA, dp ($7A) ────────────────────────────────────────────────────
    case 0x7A: {
        uint8_t  d  = spcFetch();
        uint16_t ya = (uint16_t)((spcY_ << 8) | spcA_);
        uint16_t v  = (uint16_t)(spcRead(dpAddr(d)) | (spcRead(dpAddr((uint8_t)(d+1))) << 8));
        uint32_t r  = (uint32_t)(ya + v);
        spcA_ = (uint8_t)(r & 0xFF);
        spcY_ = (uint8_t)((r >> 8) & 0xFF);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_V|FL_H|FL_Z|FL_C))
            | (r > 0xFFFF ? FL_C : 0)
            | ((r&0xFFFF)==0 ? FL_Z : 0)
            | (r & 0x8000 ? FL_N : 0));
        break;
    }

    // ── SUBW YA, dp ($9A) ────────────────────────────────────────────────────
    case 0x9A: {
        uint8_t  d  = spcFetch();
        uint16_t ya = (uint16_t)((spcY_ << 8) | spcA_);
        uint16_t v  = (uint16_t)(spcRead(dpAddr(d)) | (spcRead(dpAddr((uint8_t)(d+1))) << 8));
        uint32_t r  = (uint32_t)(ya - v);
        spcA_ = (uint8_t)(r & 0xFF);
        spcY_ = (uint8_t)((r >> 8) & 0xFF);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_V|FL_H|FL_Z|FL_C))
            | (ya >= v ? FL_C : 0)
            | ((r&0xFFFF)==0 ? FL_Z : 0)
            | (r & 0x8000 ? FL_N : 0));
        break;
    }

    // ── CMPW YA, dp ($5A) ────────────────────────────────────────────────────
    case 0x5A: {
        uint8_t  d  = spcFetch();
        uint16_t ya = (uint16_t)((spcY_ << 8) | spcA_);
        uint16_t v  = (uint16_t)(spcRead(dpAddr(d)) | (spcRead(dpAddr((uint8_t)(d+1))) << 8));
        uint32_t r  = (uint32_t)(ya - v);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (ya >= v ? FL_C : 0)
            | ((r&0xFFFF)==0 ? FL_Z : 0)
            | (r & 0x8000 ? FL_N : 0));
        break;
    }

    // ── MOV dp, dp ($CA — это и MOV !abs, Y) ─────────────────────────────────
    // $CA = MOV1 m.b, C — записать флаг C в бит b ячейки (адрес=13 бит, бит=3 бита)
    case 0xCA: {
        uint16_t o = absAddr(); uint16_t a = (uint16_t)(o & 0x1FFF); uint8_t bit = (uint8_t)(o >> 13);
        uint8_t v = spcRead(a);
        if (spcPSW_ & FL_C) v |= (uint8_t)(1 << bit); else v &= (uint8_t)~(1 << bit);
        spcWrite(a, v); break;
    }

    // ── MOV X, #imm ($CD уже есть) / MOV Y, !abs ($EC) ──────────────────────
    case 0xEC: { uint16_t a = absAddr(); spcY_ = spcRead(a); setNZ(spcY_); break; }

    // ── NOTC ($ED) — инвертировать C ─────────────────────────────────────────
    case 0xED: spcPSW_ ^= FL_C; break;

    // ── DEC dp ($8B уже) / INC X dp ($3D уже) ────────────────────────────────
    // Заглушки для MOV abs, A ($C5 уже), MOV abs, X ($C9):
    case 0xC9: { uint16_t a = absAddr(); spcWrite(a, spcX_); break; }

    // ── DBNZ dp, rel ($6E) ────────────────────────────────────────────────────
    case 0x6E: {
        uint8_t d   = spcFetch(); int8_t off = (int8_t)spcFetch();
        uint16_t a  = dpAddr(d);
        uint8_t  v  = (uint8_t)(spcRead(a) - 1);
        spcWrite(a, v);
        if (v != 0) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }

    // ── AND A, dp ($24) ──────────────────────────────────────────────────────
    case 0x24: { uint8_t d = spcFetch(); spcA_ &= spcRead(dpAddr(d)); setNZ(spcA_); break; }
    // ── OR A, dp ($04) ───────────────────────────────────────────────────────
    case 0x04: { uint8_t d = spcFetch(); spcA_ |= spcRead(dpAddr(d)); setNZ(spcA_); break; }
    // ── EOR A, dp ($44) ──────────────────────────────────────────────────────
    case 0x44: { uint8_t d = spcFetch(); spcA_ ^= spcRead(dpAddr(d)); setNZ(spcA_); break; }
    // ── ADC A, dp ($84) ──────────────────────────────────────────────────────
    case 0x84: { uint8_t d = spcFetch(); spcA_ = adc8(spcA_, spcRead(dpAddr(d))); break; }
    // ── SBC A, dp ($A4) ──────────────────────────────────────────────────────
    case 0xA4: { uint8_t d = spcFetch(); spcA_ = sbc8(spcA_, spcRead(dpAddr(d))); break; }

    // ── ASL dp ($0B) ─────────────────────────────────────────────────────────
    case 0x0B: {
        uint8_t d = spcFetch(); uint16_t a = dpAddr(d); uint8_t v = spcRead(a);
        spcPSW_ = (uint8_t)((spcPSW_ & ~FL_C) | (v >> 7));
        v <<= 1; spcWrite(a, v); setNZ(v); break;
    }
    // ── LSR dp ($4B) ─────────────────────────────────────────────────────────
    case 0x4B: {
        uint8_t d = spcFetch(); uint16_t a = dpAddr(d); uint8_t v = spcRead(a);
        spcPSW_ = (uint8_t)((spcPSW_ & ~FL_C) | (v & 1));
        v >>= 1; spcWrite(a, v); setNZ(v); break;
    }
    // ── ROL dp ($2B) ─────────────────────────────────────────────────────────
    case 0x2B: {
        uint8_t d = spcFetch(); uint16_t a = dpAddr(d); uint8_t v = spcRead(a);
        uint8_t c = spcPSW_ & FL_C;
        spcPSW_ = (uint8_t)((spcPSW_ & ~FL_C) | (v >> 7));
        v = (uint8_t)((v << 1) | c); spcWrite(a, v); setNZ(v); break;
    }
    // ── ROR dp ($6B) ─────────────────────────────────────────────────────────
    case 0x6B: {
        uint8_t d = spcFetch(); uint16_t a = dpAddr(d); uint8_t v = spcRead(a);
        uint8_t c = spcPSW_ & FL_C;
        spcPSW_ = (uint8_t)((spcPSW_ & ~FL_C) | (v & 1));
        v = (uint8_t)((v >> 1) | (c << 7)); spcWrite(a, v); setNZ(v); break;
    }

    // ── CBNE dp+X, rel ($DE) ─────────────────────────────────────────────────
    case 0xDE: {
        uint8_t d = spcFetch(); int8_t off = (int8_t)spcFetch();
        if (spcA_ != spcRead(dpAddr((uint8_t)(d+spcX_)))) spcPC_ = (uint16_t)(spcPC_ + off);
        break;
    }

    // ── MOV !abs+X, A ($D5) ──────────────────────────────────────────────────
    case 0xD5: { uint16_t a = absAddr(); spcWrite((uint16_t)(a + spcX_), spcA_); break; }
    // ── MOV !abs+Y, A ($D6) ──────────────────────────────────────────────────
    case 0xD6: { uint16_t a = absAddr(); spcWrite((uint16_t)(a + spcY_), spcA_); break; }
    // ── MOV A, !abs+X ($F5) ──────────────────────────────────────────────────
    case 0xF5: { uint16_t a = absAddr(); spcA_ = spcRead((uint16_t)(a + spcX_)); setNZ(spcA_); break; }
    // ── MOV A, !abs+Y ($F6) ──────────────────────────────────────────────────
    case 0xF6: { uint16_t a = absAddr(); spcA_ = spcRead((uint16_t)(a + spcY_)); setNZ(spcA_); break; }
    // ── MOV X, !abs ($E9) ─────────────────────────────────────────────────────
    case 0xE9: { uint16_t a = absAddr(); spcX_ = spcRead(a); setNZ(spcX_); break; }
    // ── MOV !abs, Y ($CC) ─────────────────────────────────────────────────────
    case 0xCC: { uint16_t a = absAddr(); spcWrite(a, spcY_); break; }
    // ── MOV X, dp+Y ($F9) ────────────────────────────────────────────────────
    case 0xF9: { uint8_t d = spcFetch(); spcX_ = spcRead(dpAddr((uint8_t)(d+spcY_))); setNZ(spcX_); break; }
    // ── MOV dp+Y, X ($D9) ─ хранит X (по карте опкодов SPC700) ────────────────
    case 0xD9: { uint8_t d = spcFetch(); spcWrite(dpAddr((uint8_t)(d+spcY_)), spcX_); break; }
    // ── MOV dp+X, Y ($DB) ─ хранит Y (ранее отсутствовал → рассинхрон потока) ──
    case 0xDB: { uint8_t d = spcFetch(); spcWrite(dpAddr((uint8_t)(d+spcX_)), spcY_); break; }
    // ── CMP X, !abs ($1E) ────────────────────────────────────────────────────
    case 0x1E: {
        uint16_t a = absAddr(); uint8_t v = spcRead(a);
        uint8_t r = (uint8_t)(spcX_ - v);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcX_ >= v ? FL_C : 0) | (r == 0 ? FL_Z : 0) | (r & 0x80 ? FL_N : 0));
        break;
    }
    // ── CMP Y, !abs ($5E) ────────────────────────────────────────────────────
    case 0x5E: {
        uint16_t a = absAddr(); uint8_t v = spcRead(a);
        uint8_t r = (uint8_t)(spcY_ - v);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcY_ >= v ? FL_C : 0) | (r == 0 ? FL_Z : 0) | (r & 0x80 ? FL_N : 0));
        break;
    }
    // ── CMP X, dp ($3E) ──────────────────────────────────────────────────────
    case 0x3E: {
        uint8_t d = spcFetch(); uint8_t v = spcRead(dpAddr(d));
        uint8_t r = (uint8_t)(spcX_ - v);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcX_ >= v ? FL_C : 0) | (r == 0 ? FL_Z : 0) | (r & 0x80 ? FL_N : 0));
        break;
    }
    // ── CMP A, !abs ($65) ────────────────────────────────────────────────────
    case 0x65: {
        uint16_t a = absAddr(); uint8_t v = spcRead(a);
        uint8_t r = (uint8_t)(spcA_ - v);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (spcA_ >= v ? FL_C : 0) | (r == 0 ? FL_Z : 0) | (r & 0x80 ? FL_N : 0));
        break;
    }
    // ── INCW dp ($3A) / DECW dp ($1A) — 16-битные dp-инкременты ─────────────
    case 0x3A: {
        uint8_t d = spcFetch();
        uint16_t v = (uint16_t)(spcRead(dpAddr(d)) | (spcRead(dpAddr((uint8_t)(d+1))) << 8));
        v = (uint16_t)(v + 1);
        spcWrite(dpAddr(d), (uint8_t)(v & 0xFF));
        spcWrite(dpAddr((uint8_t)(d+1)), (uint8_t)(v >> 8));
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z))
            | (v == 0 ? FL_Z : 0) | (v & 0x8000 ? FL_N : 0));
        break;
    }
    case 0x1A: {
        uint8_t d = spcFetch();
        uint16_t v = (uint16_t)(spcRead(dpAddr(d)) | (spcRead(dpAddr((uint8_t)(d+1))) << 8));
        v = (uint16_t)(v - 1);
        spcWrite(dpAddr(d), (uint8_t)(v & 0xFF));
        spcWrite(dpAddr((uint8_t)(d+1)), (uint8_t)(v >> 8));
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z))
            | (v == 0 ? FL_Z : 0) | (v & 0x8000 ? FL_N : 0));
        break;
    }
    // ── PCALL u ($4F) — call $FFxx ────────────────────────────────────────
    case 0x4F: {
        uint8_t u = spcFetch();
        spcPush((uint8_t)(spcPC_ >> 8));
        spcPush((uint8_t)(spcPC_));
        spcPC_ = (uint16_t)(0xFF00 | u);
        break;
    }
    // ── BVC/BVS ($50/$70) ─────────────────────────────────────────────────
    case 0x50: { int8_t off = (int8_t)spcFetch(); if (!(spcPSW_ & FL_V)) spcPC_ = (uint16_t)(spcPC_ + off); break; }
    case 0x70: { int8_t off = (int8_t)spcFetch(); if   (spcPSW_ & FL_V)  spcPC_ = (uint16_t)(spcPC_ + off); break; }
    // ── BCC уже есть как $90, BCS как $B0 ─────────────────────────────────
    // ── ADC dp, dp ($89) / SBC dp, dp ($A9) ───────────────────────────────
    case 0x89: {
        uint8_t s = spcFetch(); uint8_t d = spcFetch();
        spcWrite(dpAddr(d), adc8(spcRead(dpAddr(d)), spcRead(dpAddr(s))));
        break;
    }
    case 0xA9: {
        uint8_t s = spcFetch(); uint8_t d = spcFetch();
        spcWrite(dpAddr(d), sbc8(spcRead(dpAddr(d)), spcRead(dpAddr(s))));
        break;
    }
    // ── OR/AND/EOR/CMP/ADC/SBC (X),(Y) ─────────────────────────────────────
    case 0x19: { // OR (X), (Y): mem[X] |= mem[Y]
        uint16_t ax = dpAddr(spcX_), ay = dpAddr(spcY_);
        uint8_t v = (uint8_t)(spcRead(ax) | spcRead(ay));
        spcWrite(ax, v); setNZ(v);
        break;
    }
    case 0x39: {
        uint16_t ax = dpAddr(spcX_), ay = dpAddr(spcY_);
        uint8_t v = (uint8_t)(spcRead(ax) & spcRead(ay));
        spcWrite(ax, v); setNZ(v);
        break;
    }
    case 0x59: {
        uint16_t ax = dpAddr(spcX_), ay = dpAddr(spcY_);
        uint8_t v = (uint8_t)(spcRead(ax) ^ spcRead(ay));
        spcWrite(ax, v); setNZ(v);
        break;
    }
    case 0x79: {
        uint8_t vx = spcRead(dpAddr(spcX_)), vy = spcRead(dpAddr(spcY_));
        uint8_t r = (uint8_t)(vx - vy);
        spcPSW_ = (uint8_t)((spcPSW_ & ~(FL_N|FL_Z|FL_C))
            | (vx >= vy ? FL_C : 0) | (r == 0 ? FL_Z : 0) | (r & 0x80 ? FL_N : 0));
        break;
    }
    // ── RETI ($7F) ────────────────────────────────────────────────────────
    case 0x7F: {
        spcPSW_ = spcPop();
        uint8_t lo = spcPop(), hi = spcPop();
        spcPC_ = (uint16_t)(lo | (hi << 8));
        break;
    }
    // ── ORA A, addr (недостающие режимы адресации) ────────────────────────
    case 0x14: spcA_ |= spcRead(aDpX());  setNZ(spcA_); break; // ORA A,dp+X
    case 0x05: spcA_ |= spcRead(absAddr()); setNZ(spcA_); break; // ORA A,abs
    case 0x15: spcA_ |= spcRead(aAbsX()); setNZ(spcA_); break; // ORA A,abs+X
    case 0x16: spcA_ |= spcRead(aAbsY()); setNZ(spcA_); break; // ORA A,abs+Y
    case 0x06: spcA_ |= spcRead(dpAddr(spcX_)); setNZ(spcA_); break; // ORA A,(X)
    case 0x07: spcA_ |= spcRead(aIndX()); setNZ(spcA_); break; // ORA A,[dp+X]
    case 0x17: spcA_ |= spcRead(aIndY()); setNZ(spcA_); break; // ORA A,[dp]+Y
    case 0x09: { uint8_t s=spcFetch(); uint8_t d=spcFetch();   // ORA dp,dp
        uint8_t r=(uint8_t)(spcRead(dpAddr(d))|spcRead(dpAddr(s))); spcWrite(dpAddr(d),r); setNZ(r); break; }

    // ── AND A, addr (недостающие режимы адресации) ────────────────────────
    case 0x34: spcA_ &= spcRead(aDpX());  setNZ(spcA_); break; // AND A,dp+X
    case 0x25: spcA_ &= spcRead(absAddr()); setNZ(spcA_); break; // AND A,abs
    case 0x35: spcA_ &= spcRead(aAbsX()); setNZ(spcA_); break; // AND A,abs+X
    case 0x36: spcA_ &= spcRead(aAbsY()); setNZ(spcA_); break; // AND A,abs+Y
    case 0x26: spcA_ &= spcRead(dpAddr(spcX_)); setNZ(spcA_); break; // AND A,(X)
    case 0x27: spcA_ &= spcRead(aIndX()); setNZ(spcA_); break; // AND A,[dp+X]
    case 0x37: spcA_ &= spcRead(aIndY()); setNZ(spcA_); break; // AND A,[dp]+Y
    case 0x29: { uint8_t s=spcFetch(); uint8_t d=spcFetch();   // AND dp,dp
        uint8_t r=(uint8_t)(spcRead(dpAddr(d))&spcRead(dpAddr(s))); spcWrite(dpAddr(d),r); setNZ(r); break; }

    // ── EOR A, addr (недостающие режимы адресации) ────────────────────────
    case 0x54: spcA_ ^= spcRead(aDpX());  setNZ(spcA_); break; // EOR A,dp+X
    case 0x45: spcA_ ^= spcRead(absAddr()); setNZ(spcA_); break; // EOR A,abs
    case 0x55: spcA_ ^= spcRead(aAbsX()); setNZ(spcA_); break; // EOR A,abs+X
    case 0x56: spcA_ ^= spcRead(aAbsY()); setNZ(spcA_); break; // EOR A,abs+Y
    case 0x46: spcA_ ^= spcRead(dpAddr(spcX_)); setNZ(spcA_); break; // EOR A,(X)
    case 0x47: spcA_ ^= spcRead(aIndX()); setNZ(spcA_); break; // EOR A,[dp+X]
    case 0x57: spcA_ ^= spcRead(aIndY()); setNZ(spcA_); break; // EOR A,[dp]+Y
    case 0x49: { uint8_t s=spcFetch(); uint8_t d=spcFetch();   // EOR dp,dp
        uint8_t r=(uint8_t)(spcRead(dpAddr(d))^spcRead(dpAddr(s))); spcWrite(dpAddr(d),r); setNZ(r); break; }

    // ── CMP A, addr (недостающие режимы адресации) ────────────────────────
    case 0x74: cmp8(spcA_, spcRead(aDpX()));  break; // CMP A,dp+X
    case 0x75: cmp8(spcA_, spcRead(aAbsX())); break; // CMP A,abs+X
    case 0x76: cmp8(spcA_, spcRead(aAbsY())); break; // CMP A,abs+Y
    case 0x66: cmp8(spcA_, spcRead(dpAddr(spcX_))); break; // CMP A,(X)
    case 0x67: cmp8(spcA_, spcRead(aIndX())); break; // CMP A,[dp+X]
    case 0x77: cmp8(spcA_, spcRead(aIndY())); break; // CMP A,[dp]+Y
    case 0x69: { uint8_t s=spcFetch(); uint8_t d=spcFetch();   // CMP dp,dp
        cmp8(spcRead(dpAddr(d)), spcRead(dpAddr(s))); break; }

    // ── Сдвиги/вращения abs ($0C ASL,$4C LSR,$2C ROL,$6C ROR) ─────────────────
    case 0x0C: { uint16_t a=absAddr(); uint8_t v=spcRead(a); spcPSW_=(uint8_t)((spcPSW_&~FL_C)|(v>>7)); v<<=1; spcWrite(a,v); setNZ(v); break; }
    case 0x4C: { uint16_t a=absAddr(); uint8_t v=spcRead(a); spcPSW_=(uint8_t)((spcPSW_&~FL_C)|(v&1)); v>>=1; spcWrite(a,v); setNZ(v); break; }
    case 0x2C: { uint16_t a=absAddr(); uint8_t v=spcRead(a); uint8_t c=spcPSW_&FL_C; spcPSW_=(uint8_t)((spcPSW_&~FL_C)|(v>>7)); v=(uint8_t)((v<<1)|c); spcWrite(a,v); setNZ(v); break; }
    case 0x6C: { uint16_t a=absAddr(); uint8_t v=spcRead(a); uint8_t c=spcPSW_&FL_C; spcPSW_=(uint8_t)((spcPSW_&~FL_C)|(v&1)); v=(uint8_t)((v>>1)|(c<<7)); spcWrite(a,v); setNZ(v); break; }

    // ── Сдвиги/вращения dp+X ($1B ASL,$5B LSR,$3B ROL,$7B ROR) ────────────────
    case 0x1B: { uint16_t a=dpAddr((uint8_t)(spcFetch()+spcX_)); uint8_t v=spcRead(a); spcPSW_=(uint8_t)((spcPSW_&~FL_C)|(v>>7)); v<<=1; spcWrite(a,v); setNZ(v); break; }
    case 0x5B: { uint16_t a=dpAddr((uint8_t)(spcFetch()+spcX_)); uint8_t v=spcRead(a); spcPSW_=(uint8_t)((spcPSW_&~FL_C)|(v&1)); v>>=1; spcWrite(a,v); setNZ(v); break; }
    case 0x3B: { uint16_t a=dpAddr((uint8_t)(spcFetch()+spcX_)); uint8_t v=spcRead(a); uint8_t c=spcPSW_&FL_C; spcPSW_=(uint8_t)((spcPSW_&~FL_C)|(v>>7)); v=(uint8_t)((v<<1)|c); spcWrite(a,v); setNZ(v); break; }
    case 0x7B: { uint16_t a=dpAddr((uint8_t)(spcFetch()+spcX_)); uint8_t v=spcRead(a); uint8_t c=spcPSW_&FL_C; spcPSW_=(uint8_t)((spcPSW_&~FL_C)|(v&1)); v=(uint8_t)((v>>1)|(c<<7)); spcWrite(a,v); setNZ(v); break; }

    // ── MOV [dp+X], A ($C7) — косвенно-индексное хранение ──────────────────────
    case 0xC7: { uint8_t d=(uint8_t)(spcFetch()+spcX_);
        uint16_t a=(uint16_t)(spcRead(dpAddr(d))|(spcRead(dpAddr((uint8_t)(d+1)))<<8));
        spcWrite(a, spcA_); break; }

    // ── Битовые операции с флагом C (адрес=13 бит, бит=3 бита) ────────────────
    // OR1/AND1/EOR1/MOV1 C,m.b — влияют только на C; NOT1 m.b — без флагов.
    case 0x0A: { uint16_t o=absAddr(); bool b=((spcRead((uint16_t)(o&0x1FFF))>>(o>>13))&1); if(b)  spcPSW_|=FL_C; break; }            // OR1  C, m.b
    case 0x2A: { uint16_t o=absAddr(); bool b=((spcRead((uint16_t)(o&0x1FFF))>>(o>>13))&1); if(!b) spcPSW_|=FL_C; break; }            // OR1  C, /m.b
    case 0x4A: { uint16_t o=absAddr(); bool b=((spcRead((uint16_t)(o&0x1FFF))>>(o>>13))&1); if(!b) spcPSW_&=(uint8_t)~FL_C; break; }   // AND1 C, m.b
    case 0x6A: { uint16_t o=absAddr(); bool b=((spcRead((uint16_t)(o&0x1FFF))>>(o>>13))&1); if(b)  spcPSW_&=(uint8_t)~FL_C; break; }   // AND1 C, /m.b
    case 0x8A: { uint16_t o=absAddr(); bool b=((spcRead((uint16_t)(o&0x1FFF))>>(o>>13))&1); if(b)  spcPSW_^=FL_C; break; }            // EOR1 C, m.b
    case 0xAA: { uint16_t o=absAddr(); bool b=((spcRead((uint16_t)(o&0x1FFF))>>(o>>13))&1); spcPSW_=(uint8_t)((spcPSW_&~FL_C)|(b?FL_C:0)); break; } // MOV1 C, m.b
    case 0xEA: { uint16_t o=absAddr(); uint16_t a=(uint16_t)(o&0x1FFF); uint8_t bit=(uint8_t)(o>>13); uint8_t v=spcRead(a); v^=(uint8_t)(1<<bit); spcWrite(a,v); break; } // NOT1 m.b

    // ── TSET1/TCLR1 !abs ($0E/$4E): NZ по (A−mem), затем mem|=A / mem&=~A ──────
    case 0x0E: { uint16_t a=absAddr(); uint8_t v=spcRead(a); setNZ((uint8_t)(spcA_-v)); spcWrite(a,(uint8_t)(v| spcA_)); break; }
    case 0x4E: { uint16_t a=absAddr(); uint8_t v=spcRead(a); setNZ((uint8_t)(spcA_-v)); spcWrite(a,(uint8_t)(v&~spcA_)); break; }

    // ── DAA/DAS A ($DF/$BE) — десятичная коррекция аккумулятора ────────────────
    case 0xDF: { // DAA (после сложения)
        if ((spcPSW_&FL_C) || spcA_ > 0x99) { spcA_ = (uint8_t)(spcA_ + 0x60); spcPSW_ |= FL_C; }
        if ((spcPSW_&FL_H) || (spcA_ & 0x0F) > 0x09) { spcA_ = (uint8_t)(spcA_ + 0x06); }
        setNZ(spcA_); break;
    }
    case 0xBE: { // DAS (после вычитания)
        if (!(spcPSW_&FL_C) || spcA_ > 0x99) { spcA_ = (uint8_t)(spcA_ - 0x60); spcPSW_ &= (uint8_t)~FL_C; }
        if (!(spcPSW_&FL_H) || (spcA_ & 0x0F) > 0x09) { spcA_ = (uint8_t)(spcA_ - 0x06); }
        setNZ(spcA_); break;
    }

    // ── BRK ($0F): push PC,PSW; вектор $FFDE ──────────────────────────────────
    case 0x0F: {
        spcPush((uint8_t)(spcPC_ >> 8)); spcPush((uint8_t)spcPC_); spcPush(spcPSW_);
        spcPSW_ |= FL_B; spcPSW_ &= (uint8_t)~FL_I;
        spcPC_ = (uint16_t)(spcRead(0xFFDE) | (spcRead(0xFFDF) << 8));
        break;
    }

    // ── NOP-like для редких неизвестных опкодов ───────────────────────────
    default:
        break;
    }
    return 2;   // аппроксимация: средняя инструкция SPC700 ≈ 2 такта
}
