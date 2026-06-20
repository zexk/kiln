#include "frustum.h"
#include <math.h>

static void extract_plane(plane_t *p, float a, float b, float c, float d) {
    float len = sqrtf(a * a + b * b + c * c);
    p->a = a / len; p->b = b / len; p->c = c / len; p->d = d / len;
}

/* Gribb-Hartmann extraction from a column-major view-projection matrix.
   Row i = m[i], m[4+i], m[8+i], m[12+i].
   Near/far use Vulkan depth [0,1]: near is row2, far is row3-row2. */
void frustum_extract(frustum_t *f, mat4_t vp) {
    const float *m = vp.m;
    extract_plane(&f->planes[0], m[3]+m[0],  m[7]+m[4],  m[11]+m[8],  m[15]+m[12]); /* left   */
    extract_plane(&f->planes[1], m[3]-m[0],  m[7]-m[4],  m[11]-m[8],  m[15]-m[12]); /* right  */
    extract_plane(&f->planes[2], m[3]+m[1],  m[7]+m[5],  m[11]+m[9],  m[15]+m[13]); /* bottom */
    extract_plane(&f->planes[3], m[3]-m[1],  m[7]-m[5],  m[11]-m[9],  m[15]-m[13]); /* top    */
    extract_plane(&f->planes[4], m[2],        m[6],        m[10],        m[14]);       /* near   */
    extract_plane(&f->planes[5], m[3]-m[2],  m[7]-m[6],  m[11]-m[10], m[15]-m[14]); /* far    */
}

bool frustum_intersects_aabb(const frustum_t *f, vec3_t min, vec3_t max) {
    for (int i = 0; i < 6; i++) {
        const plane_t *p = &f->planes[i];
        float px = p->a > 0.0f ? max.x : min.x;
        float py = p->b > 0.0f ? max.y : min.y;
        float pz = p->c > 0.0f ? max.z : min.z;
        if (p->a * px + p->b * py + p->c * pz + p->d < 0.0f)
            return false;
    }
    return true;
}
