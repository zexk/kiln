#pragma once
#include <stdbool.h>
#include "../platform/platform.h"

#define INPUT_MAX_KEYS 256

typedef struct {
    bool  keys[INPUT_MAX_KEYS]; /* indexed by keysym & 0xFF; true while held  */
    float mouse_dx, mouse_dy;  /* accumulated relative motion this frame      */
    float mouse_x,  mouse_y;  /* absolute pointer position                   */
    bool  mouse_left;
    bool  mouse_middle;
    bool  mouse_right;
} game_input_t;

void game_input_init(game_input_t *input);
void game_input_handle_event(game_input_t *input, const event_t *event);
void game_input_end_frame(game_input_t *input);
