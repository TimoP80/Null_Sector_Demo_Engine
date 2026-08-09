// ---------------------------------------------------------------------------
// modelcheck - dev preflight for the 3D pipeline (the --check-shaders idea
// applied to the model/material path).
//
// The OBJ importer + lit shader + ModelRenderer only ever run at demo time,
// so a regression (tangent buffer sizing, face parsing, UBO binding, uniform
// reflection, material override) surfaces mid-show. checkModelPipeline() runs
// the whole chain up front: a generated cube through ObjImporter ->
// MeshPrimitive::upload -> ModelRenderer draw into an offscreen target with a
// pixel readback, plus the shipped data/models + data/materials the demo
// actually uses. Needs a GL context; never opens the show.
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

namespace ns {

struct ModelCheckResult {
  int total = 0;
  int ok = 0;
  int failed = 0;
  std::vector<std::string> failedItems;
};

/** run the full model pipeline check; returns the aggregate (never throws) */
ModelCheckResult checkModelPipeline();

}  // namespace ns
