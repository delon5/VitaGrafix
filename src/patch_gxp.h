#ifndef _PATCH_GXP_H_
#define _PATCH_GXP_H_

#include "io.h"

// Shader literal patches (gxp: and gxplit: lines, see patch_gxp_core.h)
void vg_gxp_reset();
vg_io_status_t vg_gxp_parse_patch(const char line[]);
vg_io_status_t vg_gxp_install();
void vg_gxp_uninstall();

#endif
