#include "input.h"
#include <string.h>

void game_input_init(game_input_t *input) {
    memset(input, 0, sizeof(*input));
}

void game_input_handle_event(game_input_t *input, const event_t *ev) {
    switch (ev->type) {
    case EVENT_KEY_DOWN:
        input->keys[ev->key.keysym & 0xFF] = true;
        break;
    case EVENT_KEY_UP:
        input->keys[ev->key.keysym & 0xFF] = false;
        break;
    case EVENT_MOUSE_MOTION:
        input->mouse_dx += ev->motion.dx;
        input->mouse_dy += ev->motion.dy;
        input->mouse_x   = (float)ev->motion.x;
        input->mouse_y   = (float)ev->motion.y;
        break;
    case EVENT_MOUSE_BUTTON:
        switch (ev->button.button) {
        case MOUSE_BUTTON_LEFT:   input->mouse_left   = ev->button.down; break;
        case MOUSE_BUTTON_MIDDLE: input->mouse_middle = ev->button.down; break;
        case MOUSE_BUTTON_RIGHT:  input->mouse_right  = ev->button.down; break;
        default: break;
        }
        break;
    default:
        break;
    }
}

void game_input_end_frame(game_input_t *input) {
    input->mouse_dx = 0.0f;
    input->mouse_dy = 0.0f;
}
