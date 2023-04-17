/*
 * Copyright (c) Antmicro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARCH_EXPORTS_H_
#define ARCH_EXPORTS_H_

#include <stdint.h>

uint64_t tlib_get_system_register(const char *name);

void tlib_set_available_els(bool el2_enabled, bool el3_enabled);
void tlib_set_current_el(uint32_t el);
void tlib_set_system_register(const char *name, uint64_t value);

#endif
