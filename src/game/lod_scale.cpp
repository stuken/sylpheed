// sylpheed - ReXGlue Recompiled Project
//
// sylpheed_lod_scale: multiplies every model LOD distance. The game picks a
// mesh per model part from a per-model table in DefTables.pak (LODSetID,
// Levels, [Level_N] Model / FarDistance) by the raw camera distance, with no
// detail setting of its own. Above 1 the detailed meshes stay on farther out
// and models whose last level is empty are culled later; below 1 the reverse.
// 0 always takes each set's first level, which also stops that culling.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "core/settings.h"

REXCVAR_DEFINE_DOUBLE(sylpheed_lod_scale, 1.0, kCvarGroup,
                      "Multiply model LOD distances (>1 = detailed meshes "
                      "farther out, 0 = always the most detailed)");

REX_EXTERN(__imp__Sylpheed_SelectLodMesh);

// 0x82255D00: f1 is the distance compared against each level's FarDistance,
// so dividing it scales every FarDistance at once. The motion manager's loader
// (0x82259570) also calls it with distance 0 to fetch level 0, which this
// leaves alone.
REX_HOOK_RAW(Sylpheed_SelectLodMesh) {
  const double scale = REXCVAR_GET(sylpheed_lod_scale);
  if (scale != 1.0)
    ctx.f1.f64 = scale > 0.0 ? ctx.f1.f64 / scale : 0.0;
  __imp__Sylpheed_SelectLodMesh(ctx, base);
}
