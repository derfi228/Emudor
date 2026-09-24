#pragma once
#include <cstdint>

// ─── DSP-1 (NEC uPD77C25) — математический сопроцессор SNES ────────────────────
// Используется в Super Mario Kart, Pilotwings и др. Даёт 3D-проекцию: игра
// задаёт положение камеры (Parameter), а затем построчно спрашивает матрицу
// Mode 7 (Raster) и экранные координаты объектов (Project).
//
// Интерфейс с CPU:
//   DR (Data Register)   — $6000-$6FFF: команда, затем параметры, затем результат.
//   SR (Status Register) — $7000-$7FFF: бит7 (RQM) = готов к обмену.
//
// Это HLE: команды считаются сразу и в double, а не тактами настоящего DSP.
// Наружу всё отдаётся в тех же форматах с фиксированной точкой, что и на железе.
class SnesDSP1 {
public:
    void    reset();
    uint8_t readDR();             // чтение Data Register ($6000)
    void    writeDR(uint8_t v);   // запись Data Register ($6000)
    uint8_t readSR() const;       // чтение Status Register ($7000)

    // Save state: поля перечислены один раз для записи и чтения
    // (S = StateWriter / StateReader из console/state_io.h).
    template<class S> void serialize(S& s);

private:
    // ─── Протокол обмена ──────────────────────────────────────────────────────
    uint8_t command_  = 0;
    uint8_t inBytes_  = 0;        // сколько байт параметров ещё ждём
    uint8_t inIndex_  = 0;
    uint8_t outBytes_ = 0;
    uint8_t outIndex_ = 0;
    uint8_t inBuf_[16]{};
    uint8_t outBuf_[16]{};

    int16_t arg(int i) const;             // параметр-слово i
    void    setOut(int words);            // подготовить N слов результата
    void    put(int i, int16_t v);        // записать слово результата i
    void    exec();                       // выполнить накопленную команду
    void    rasterOut();                  // матрица Mode 7 для строки rasterVs_

    // Число входных слов команды; 0xFF = команда неизвестна
    static uint8_t cmdInWords(uint8_t cmd);

    // ─── Состояние проекции (задаётся командой Parameter) ─────────────────────
    // Модель чипа: F — точка на земле, куда смотрит камера; глаз стоит от неё
    // на расстоянии Lfe вдоль единичного вектора N (назад и вверх), экран —
    // на расстоянии Les от глаза. Смотрим вдоль -N.
    double  sa_ = 0, ca_ = 1;             // sin/cos азимута Aas
    double  sz_ = 0, cz_ = 1;             // sin/cos зенитного угла Azs
    double  nx_ = 0, ny_ = 0, nz_ = 1;    // N
    double  centreX_ = 0, centreY_ = 0;   // центр экрана на земле (после Parameter)
    double  centreZ_ = 0;                 // высота глаза
    double  gx_ = 0, gy_ = 0, gz_ = 0;    // центр плоскости экрана в мире
    double  les_ = 0;                     // расстояние глаз → экран
    double  vOffset_ = 0;                 // Les * cos(Azs) (Azs ограничен)
    double  secAzs_ = 1;                  // 1 / cos(Azs) (Azs ограничен)
    int16_t rasterVs_ = 0;                // текущая строка потокового Raster

    // ─── Матрицы вращения A/B/C (Attitude → Objective/Subject/Scalar) ─────────
    double  mat_[3][3][3]{};
};
