#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    char session_reset_label[24]; // compact wall-clock reset label
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char weekly_reset_label[24];  // compact wall-clock reset label
    char status[16];         // "allowed" or "limited"
    char plan[16];           // e.g. "pro"
    char limit[24];          // e.g. "codex"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};
