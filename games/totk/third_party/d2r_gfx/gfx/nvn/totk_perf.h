// Local change: per-second performance log (totk_perf.c).
#pragma once
#include <stdint.h>
void totk_perf_frame(uint64_t frame, double frame_ms, double gpu_ms, unsigned pso, unsigned lib, double make_ms, int loading);
