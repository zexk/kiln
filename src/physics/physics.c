#include "physics.h"
#include <math.h>

/* Check the four XZ corners of the AABB column at position (x, y, z)
   against a single horizontal slab (integer row iy). */
static bool corners_solid(phys_solid_fn solid, void *ctx,
                           float x, int iy, float z, float hw) {
    return solid(ctx, (int)floorf(x - hw), iy, (int)floorf(z - hw)) ||
           solid(ctx, (int)floorf(x + hw), iy, (int)floorf(z - hw)) ||
           solid(ctx, (int)floorf(x - hw), iy, (int)floorf(z + hw)) ||
           solid(ctx, (int)floorf(x + hw), iy, (int)floorf(z + hw));
}

/* Return true if the full vertical column of the AABB at (x, oy, z) is
   clear.  A small skin (0.01) is pulled in from foot and head so the
   check passes when the player stands exactly on a block boundary. */
static bool column_clear(phys_solid_fn solid, void *ctx,
                          float x, float oy, float z,
                          float hw, float foot_off, float head_off) {
    float min_y = oy - foot_off + 0.01f;
    float max_y = oy + head_off - 0.01f;
    for (int iy = (int)floorf(min_y); iy <= (int)floorf(max_y); iy++)
        if (corners_solid(solid, ctx, x, iy, z, hw)) return false;
    return true;
}

void phys_step(phys_body_t *body, float dt, phys_solid_fn solid, void *ctx) {
    if (dt > 1.0f / 60.0f) dt = 1.0f / 60.0f;

    body->velocity.y -= body->gravity * dt;

    float hw = body->half_w;
    float ox  = body->position.x;
    float oy  = body->position.y;
    float oz  = body->position.z;

    /* X axis */
    float nx = ox + body->velocity.x * dt;
    if (!column_clear(solid, ctx, nx, oy, oz, hw, body->foot_off, body->head_off)) {
        nx = ox;
        body->velocity.x = 0.0f;
    }

    /* Y axis */
    float ny = oy + body->velocity.y * dt;
    if (body->velocity.y < 0.0f) {
        int fc = (int)floorf(ny - body->foot_off);
        if (corners_solid(solid, ctx, nx, fc, oz, hw)) {
            ny              = (float)(fc + 1) + body->foot_off;
            body->velocity.y = 0.0f;
            body->grounded  = true;
        } else {
            body->grounded = false;
        }
    } else if (body->velocity.y > 0.0f) {
        int hc = (int)floorf(ny + body->head_off);
        if (corners_solid(solid, ctx, nx, hc, oz, hw)) {
            ny              = (float)hc - body->head_off;
            body->velocity.y = 0.0f;
        }
        body->grounded = false;
    } else {
        body->grounded = false;
    }

    /* Z axis */
    float nz = oz + body->velocity.z * dt;
    if (!column_clear(solid, ctx, nx, ny, nz, hw, body->foot_off, body->head_off)) {
        nz = oz;
        body->velocity.z = 0.0f;
    }

    body->position.x = nx;
    body->position.y = ny;
    body->position.z = nz;
}
