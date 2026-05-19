#pragma once

void power_init(void);
void power_tick(void);
int  power_battery_pct(void);    // 0-100, or -1 if no battery
bool power_is_charging(void);
bool power_pwr_pressed(void);    // true once per AXP2101 PWR button short-press
bool power_shutdown_requested(void); // true once after PWR is held for 3s
void power_shutdown_now(void);    // release battery hold / request deepest sleep path
