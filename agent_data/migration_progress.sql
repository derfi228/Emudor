CREATE TABLE console_progress (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    console TEXT UNIQUE NOT NULL,
    priority INTEGER NOT NULL,           -- порядок реализации (1, 2, 3...)
    status TEXT DEFAULT 'planned',       -- planned / in_progress / done
    tests_total INTEGER DEFAULT 0,
    tests_passing INTEGER DEFAULT 0,
    pass_rate REAL DEFAULT 0.0,          -- процент проходящих тестов
    started_at TIMESTAMP,
    completed_at TIMESTAMP,
    notes TEXT
);

-- Начальные данные по твоему плану
INSERT INTO console_progress (console, priority, status, notes) VALUES
    ('NES',  1, 'done',        'Готов: 46 юнит-тестов проходят, nestest зелёный'),
    ('SNES', 2, 'in_progress', 'Главный фокус. DSP/звук — заглушка'),
    ('GB',   3, 'planned',     'Game Boy DMG'),
    ('GBC',  4, 'planned',     'Game Boy Color (надстройка над GB)'),
    ('GBA',  5, 'planned',     'Game Boy Advance, ARM7TDMI'),
    ('N64',  6, 'planned',     'Сложно: 3D, MIPS R4300i, RDP/RSP'),
    ('PS1',  7, 'planned',     'Сложно: MIPS R3000, GTE, GPU, SPU');