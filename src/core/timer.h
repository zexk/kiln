#pragma once

typedef struct { double start; } kln_timer_t;

void   kln_timer_init(kln_timer_t *t);
double kln_timer_elapsed(const kln_timer_t *t);
double kln_timer_reset(kln_timer_t *t);
double kln_timer_now(void);
