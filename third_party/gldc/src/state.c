#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <kos.h>
#include <dc/pvr.h>
#include "gldc.h"

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))
#define CLAMP( X, _MIN, _MAX )  ( (X)<(_MIN) ? (_MIN) : ((X)>(_MAX) ? (_MAX) : (X)) )

GLboolean STATE_DIRTY = GL_TRUE;

GLboolean DEPTH_TEST_ENABLED = GL_FALSE;
GLboolean DEPTH_MASK_ENABLED = GL_FALSE;

GLboolean CULLING_ENABLED = GL_FALSE;

GLboolean FOG_ENABLED        = GL_FALSE;
GLboolean ALPHA_TEST_ENABLED = GL_FALSE;

GLboolean SCISSOR_TEST_ENABLED = GL_FALSE;
GLenum SHADE_MODEL = PVR_SHADE_GOURAUD;

GLboolean BLEND_ENABLED = GL_FALSE;

GLboolean TEXTURES_ENABLED = GL_FALSE;
GLboolean AUTOSORT_ENABLED = GL_FALSE;

PolyList OP_LIST;
PolyList PT_LIST;
PolyList TR_LIST;
Viewport VIEWPORT;

static struct {
    int x;
    int y;
    int width;
    int height;
    GLboolean applied;
} scissor_rect;

void glKosInit() {
    scissor_rect.width  = vid_mode->width;
    scissor_rect.height = vid_mode->height;
    _glInitTextures();

    OP_LIST.list_type = PVR_LIST_OP_POLY;
    PT_LIST.list_type = PVR_LIST_PT_POLY;
    TR_LIST.list_type = PVR_LIST_TR_POLY;

    aligned_vector_reserve(&OP_LIST.vector, 1024 * 3);
    aligned_vector_reserve(&PT_LIST.vector,  512 * 3);
    aligned_vector_reserve(&TR_LIST.vector, 1024 * 3);
}

void glKosSwapBuffers() {
    _glApplyScissor(true);

    pvr_scene_begin();   
        if (OP_LIST.vector.size > 2) {
            pvr_list_begin(PVR_LIST_OP_POLY);
            SceneListSubmit((Vertex*)OP_LIST.vector.data, OP_LIST.vector.size);
            pvr_list_finish();
        }

        if (PT_LIST.vector.size > 2) {
            pvr_list_begin(PVR_LIST_PT_POLY);
            SceneListSubmit((Vertex*)PT_LIST.vector.data, PT_LIST.vector.size);
            pvr_list_finish();
        }

        if (TR_LIST.vector.size > 2) {
            pvr_list_begin(PVR_LIST_TR_POLY);
            SceneListSubmit((Vertex*)TR_LIST.vector.data, TR_LIST.vector.size);
            pvr_list_finish();
        }
    pvr_scene_finish();
    
    OP_LIST.vector.size = 0;
    PT_LIST.vector.size = 0;
    TR_LIST.vector.size = 0;
}

void glScissor(int x, int y, int width, int height) {
    scissor_rect.x = x;
    scissor_rect.y = y;
    scissor_rect.width = width;
    scissor_rect.height = height;
    scissor_rect.applied = false;
    _glApplyScissor(false);
}

/* Setup the hardware user clip rectangle.

   The minimum clip rectangle is a 32x32 area which is dependent on the tile
   size use by the tile accelerator. The PVR swithes off rendering to tiles
   outside or inside the defined rectangle dependant upon the 'clipmode'
   bits in the polygon header.

   Clip rectangles therefore must have a size that is some multiple of 32.

    glScissor(0, 0, 32, 32) allows only the 'tile' in the lower left
    hand corner of the screen to be modified and glScissor(0, 0, 0, 0)
    disallows modification to all 'tiles' on the screen.

    We call this in the following situations:

     - glEnable(GL_SCISSOR_TEST) is called
     - glScissor() is called
     - After glKosSwapBuffers()

    This ensures that a clip command is added to every vertex list
    at the right place, either when enabling the scissor test, or
    when the scissor test changes.
*/
void _glApplyScissor(int force) {
    /* Don't do anyting if clipping is disabled */
    if (!SCISSOR_TEST_ENABLED) return;

    /* Don't apply if we already applied - nothing changed */
    if (scissor_rect.applied && !force) return;

    PVRTileClipCommand c;

    int sx, sy, ex, ey;
    int scissor_width  = MAX(MIN(scissor_rect.width,  vid_mode->width),  0);
    int scissor_height = MAX(MIN(scissor_rect.height, vid_mode->height), 0);

    /* force the origin to the lower left-hand corner of the screen */
	sx = scissor_rect.x;
    sy = (vid_mode->height - scissor_height) - scissor_rect.y;
    ex = sx + scissor_width;
    ey = sy + scissor_height;

    /* load command structure while mapping screen coords to TA tiles */
    c.flags = PVR_CMD_USERCLIP;
    c.d1 = c.d2 = c.d3 = 0;

    uint16_t vw = vid_mode->width  >> 5;
    uint16_t vh = vid_mode->height >> 5;

    c.sx = CLAMP(sx >> 5, 0, vw);
    c.sy = CLAMP(sy >> 5, 0, vh);
    c.ex = CLAMP((ex >> 5) - 1, 0, vw);
    c.ey = CLAMP((ey >> 5) - 1, 0, vh);

    aligned_vector_push_back(&OP_LIST.vector, &c, 1);
    aligned_vector_push_back(&PT_LIST.vector, &c, 1);
    aligned_vector_push_back(&TR_LIST.vector, &c, 1);

    scissor_rect.applied = true;
}


void apply_poly_header(pvr_poly_hdr_t* dst, int list_type) {
    TextureObject* tx1 = TEXTURE_ACTIVE;
    uint32_t txr_base;
    int gen_color_clamp = PVR_CLRCLAMP_DISABLE;

    int gen_culling = CULLING_ENABLED    ? PVR_CULLING_CW : PVR_CULLING_SMALL;
    int depth_comp  = DEPTH_TEST_ENABLED ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS;
    int depth_write = DEPTH_MASK_ENABLED ? PVR_DEPTHWRITE_ENABLE : PVR_DEPTHWRITE_DISABLE;

    int gen_clip_mode = SCISSOR_TEST_ENABLED ? PVR_USERCLIP_INSIDE : PVR_USERCLIP_DISABLE;
    int gen_fog_type  = FOG_ENABLED          ? PVR_FOG_TABLE : PVR_FOG_DISABLE;

    int gen_alpha = (BLEND_ENABLED || ALPHA_TEST_ENABLED) ? PVR_ALPHA_ENABLE : PVR_ALPHA_DISABLE;
    int blend_src = PVR_BLEND_SRCALPHA;
    int blend_dst = PVR_BLEND_INVSRCALPHA;

    if (list_type == PVR_LIST_OP_POLY) {
        /* Opaque polys are always one/zero */
        blend_src  = PVR_BLEND_ONE;
        blend_dst  = PVR_BLEND_ZERO;
    } else if (list_type == PVR_LIST_PT_POLY) {
        /* Punch-through polys require fixed blending and depth modes */
        blend_src  = PVR_BLEND_SRCALPHA;
        blend_dst  = PVR_BLEND_INVSRCALPHA;
        depth_comp = PVR_DEPTHCMP_LEQUAL;
    } else if (list_type == PVR_LIST_TR_POLY && AUTOSORT_ENABLED) {
        /* Autosort mode requires this mode for transparent polys */
        depth_comp = PVR_DEPTHCMP_GEQUAL;
    }

    int txr_enable, txr_alpha;
    if (!TEXTURES_ENABLED || !tx1 || !tx1->data) {
        /* Disable all texturing to start with */
        txr_enable = PVR_TEXTURE_DISABLE;
    } else {
        txr_alpha  = (BLEND_ENABLED || ALPHA_TEST_ENABLED) ? PVR_TXRALPHA_ENABLE : PVR_TXRALPHA_DISABLE;
        txr_enable = PVR_TEXTURE_ENABLE;
    }

    /* The base values for CMD */
    dst->cmd = PVR_CMD_POLYHDR;
    dst->cmd |= txr_enable << 3;
    /* Force bits 18 and 19 on to switch to 6 triangle strips */
    dst->cmd |= 0xC0000;

    /* Or in the list type, shading type, color and UV formats */
    dst->cmd |= (list_type             << PVR_TA_CMD_TYPE_SHIFT)     & PVR_TA_CMD_TYPE_MASK;
    dst->cmd |= (PVR_CLRFMT_ARGBPACKED << PVR_TA_CMD_CLRFMT_SHIFT)   & PVR_TA_CMD_CLRFMT_MASK;
    dst->cmd |= (SHADE_MODEL           << PVR_TA_CMD_SHADE_SHIFT)    & PVR_TA_CMD_SHADE_MASK;
    dst->cmd |= (PVR_UVFMT_32BIT       << PVR_TA_CMD_UVFMT_SHIFT)    & PVR_TA_CMD_UVFMT_MASK;
    dst->cmd |= (gen_clip_mode         << PVR_TA_CMD_USERCLIP_SHIFT) & PVR_TA_CMD_USERCLIP_MASK;

    /* Polygon mode 1 */
    dst->mode1  = (depth_comp  << PVR_TA_PM1_DEPTHCMP_SHIFT)   & PVR_TA_PM1_DEPTHCMP_MASK;
    dst->mode1 |= (gen_culling << PVR_TA_PM1_CULLING_SHIFT)    & PVR_TA_PM1_CULLING_MASK;
    dst->mode1 |= (depth_write << PVR_TA_PM1_DEPTHWRITE_SHIFT) & PVR_TA_PM1_DEPTHWRITE_MASK;
    dst->mode1 |= (txr_enable  << PVR_TA_PM1_TXRENABLE_SHIFT)  & PVR_TA_PM1_TXRENABLE_MASK;

    /* Polygon mode 2 */
    dst->mode2  = (blend_src       << PVR_TA_PM2_SRCBLEND_SHIFT) & PVR_TA_PM2_SRCBLEND_MASK;
    dst->mode2 |= (blend_dst       << PVR_TA_PM2_DSTBLEND_SHIFT) & PVR_TA_PM2_DSTBLEND_MASK;
    dst->mode2 |= (gen_fog_type    << PVR_TA_PM2_FOG_SHIFT)      & PVR_TA_PM2_FOG_MASK;
    dst->mode2 |= (gen_color_clamp << PVR_TA_PM2_CLAMP_SHIFT)    & PVR_TA_PM2_CLAMP_MASK;
    dst->mode2 |= (gen_alpha       << PVR_TA_PM2_ALPHA_SHIFT)    & PVR_TA_PM2_ALPHA_MASK;

    if (txr_enable == PVR_TEXTURE_DISABLE) {
        dst->mode3 = 0;
    } else {
        GLuint filter = PVR_FILTER_NEAREST;
        if (tx1->minFilter == GL_LINEAR && tx1->magFilter == GL_LINEAR) filter = PVR_FILTER_BILINEAR;

        dst->mode2 |= (txr_alpha                << PVR_TA_PM2_TXRALPHA_SHIFT) & PVR_TA_PM2_TXRALPHA_MASK;
        dst->mode2 |= (filter                   << PVR_TA_PM2_FILTER_SHIFT)   & PVR_TA_PM2_FILTER_MASK;
        dst->mode2 |= (tx1->mipmap_bias         << PVR_TA_PM2_MIPBIAS_SHIFT)  & PVR_TA_PM2_MIPBIAS_MASK;
        dst->mode2 |= (PVR_TXRENV_MODULATEALPHA << PVR_TA_PM2_TXRENV_SHIFT)   & PVR_TA_PM2_TXRENV_MASK;

        dst->mode2 |= (DimensionFlag(tx1->width)  << PVR_TA_PM2_USIZE_SHIFT) & PVR_TA_PM2_USIZE_MASK;
        dst->mode2 |= (DimensionFlag(tx1->height) << PVR_TA_PM2_VSIZE_SHIFT) & PVR_TA_PM2_VSIZE_MASK;

        /* Polygon mode 3 */
        dst->mode3  = (GL_FALSE   << PVR_TA_PM3_MIPMAP_SHIFT) & PVR_TA_PM3_MIPMAP_MASK;
        dst->mode3 |= (tx1->color << PVR_TA_PM3_TXRFMT_SHIFT) & PVR_TA_PM3_TXRFMT_MASK;

        /* Convert the texture address */
        txr_base = (uint32_t)tx1->data;
        txr_base = (txr_base & 0x00fffff8) >> 3;
        dst->mode3 |= txr_base;
    }

    dst->d1 = dst->d2 = 0xffffffff;
    dst->d3 = dst->d4 = 0xffffffff;
}
