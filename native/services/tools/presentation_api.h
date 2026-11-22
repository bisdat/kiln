// SPDX-License-Identifier: MIT
#pragma once
#include "kiln_api.h"
#include "services/kiln_tools.h"
KilnResult KILN_CALL kiln_tools_define_family(KilnOwner owner,const KilnToolFamilyDef* definition,KilnHandle* out);
KilnResult KILN_CALL kiln_tools_remove_family(KilnOwner owner,KilnHandle handle);
KilnResult KILN_CALL kiln_tools_material_snapshot(KilnEntity actor,uint32_t tool,KilnToolMaterialSnapshot* out);
KilnResult KILN_CALL kiln_tools_presentation_status(KilnToolPresentationStatus* out);
void kiln_tools_presentation_install(const KilnApi* api,KilnOwner parent);
void kiln_tools_presentation_uninstall();
