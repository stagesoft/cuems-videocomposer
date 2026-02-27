/*
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Copyright (C) 2020-2026 Stage Lab Coop.
 * Author: Ion Reguera <ion@stagelab.coop>
 *
 * This file is part of cuems-videocomposer.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef VIDEOCOMPOSER_TIME_UTILS_H
#define VIDEOCOMPOSER_TIME_UTILS_H

#ifdef __cplusplus
#include <cstdint>
extern "C" {
#else
#include <stdint.h>
#endif

/**
 * Get monotonic time in microseconds
 * 
 * Returns a monotonic clock value that always increases and doesn't suffer
 * discontinuities when the system time changes.
 * 
 * On Linux, uses CLOCK_MONOTONIC via std::chrono.
 * 
 * @return Monotonic time in microseconds
 */
int64_t vc_get_monotonic_time(void);

#ifdef __cplusplus
}
#endif

#endif // VIDEOCOMPOSER_TIME_UTILS_H

