/* weak_libjack.h - Weak linking to JACK library
 *
 * This header provides stubs for JACK functions when JACK is not available
 * or when weak linking is used.
 *
 * Copyright (C) 2024 stagelab.coop
 * Based on xjadeo code
 */

#ifndef WEAK_LIBJACK_H
#define WEAK_LIBJACK_H

// JACK support has been removed from cuems-videocomposer
// This header provides empty stubs to prevent compilation errors
// All JACK functionality has been removed

// Empty stub - JACK is not used
#define USE_WEAK_JACK 0

#endif /* WEAK_LIBJACK_H */

