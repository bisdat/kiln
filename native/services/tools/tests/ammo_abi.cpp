// SPDX-License-Identifier: MIT
#include "services/kiln_ui_ammo.h"
#include <cstddef>
static_assert(sizeof(KilnUiAmmoRow)==16);
static_assert(sizeof(KilnUiAmmoPublish)==40);
static_assert(sizeof(KilnUiAmmoContext)==1064);
static_assert(sizeof(KilnUiAmmoPick)==32);
static_assert(sizeof(KilnUiAmmoApi)==32);
static_assert(offsetof(KilnUiAmmoApi,publish)==8);
static_assert(offsetof(KilnUiAmmoApi,context)==16);
static_assert(offsetof(KilnUiAmmoApi,take_picks)==24);
int main() {}
