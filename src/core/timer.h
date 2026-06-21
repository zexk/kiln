#pragma once

typedef struct { double start; } kln_timer_t;

void   kln_timer_init(kln_timer_t *t);
double kln_timer_elapsed(const kln_timer_t *t);
double kln_timer_reset(kln_timer_t *t);
double kln_timer_now(void);
/* Sleep for approximately `seconds`. Sleeps coarsely then busy-waits for the
   last millisecond, trading a small CPU burn for < 100 µs overshoot. */
void   kln_timer_sleep(double seconds);
