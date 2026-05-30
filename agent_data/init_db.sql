CREATE TABLE issues (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    console TEXT NOT NULL,
    component TEXT,
    severity TEXT,
    status TEXT DEFAULT 'open',
    title TEXT NOT NULL,
    description TEXT,
    reproduction TEXT,
    expected TEXT,
    actual TEXT,
    screenshot_path TEXT,
    discovered_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    fixed_in_commit TEXT,
    verified_at TIMESTAMP
);

CREATE TABLE test_runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    console TEXT,
    rom_name TEXT,
    result TEXT,
    run_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    commit_hash TEXT,
    screenshot_path TEXT
);

CREATE TABLE questions_for_user (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    asked_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    agent TEXT,
    question TEXT,
    options TEXT,
    user_answer TEXT,
    answered_at TIMESTAMP
);