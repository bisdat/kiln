// SPDX-License-Identifier: MIT
#pragma once
#include "engine.h"
namespace kitems {
void storage_init();
void storage_ready();
void storage_pre(uint64_t step);
void storage_program(const Access&);
bool storage_work();
void storage_cancel_owner(KilnOwner);
void storage_reset();
} // namespace kitems
