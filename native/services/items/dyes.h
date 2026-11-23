// SPDX-License-Identifier: MIT
#pragma once
#include "services/kiln_stations.h"
namespace kitems {
void dyes_resolve();
void dyes_register();
KilnResult KILN_CALL dyes_set_stock(KilnEntity player,const KilnStationStack* rows,uint32_t count);
}
