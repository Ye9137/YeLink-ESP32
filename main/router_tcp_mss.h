// Copyright (c) 2026 Ye
#pragma once
#include <stddef.h>
#include <stdint.h>
// IPv4 SYN/SYN-ACK only. No allocation, insertion, or change to smaller MSS.
// Returns the number of modified MSS options; malformed headers stay untouched.
unsigned router_tcp_clamp_mss(uint8_t *ip, size_t contiguous, size_t total, uint16_t limit);
