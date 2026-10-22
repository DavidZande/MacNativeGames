// TOTK runtime shader translation (totk_shader_rt.m).
#pragma once
#ifdef __OBJC__
#import <Foundation/Foundation.h>
#include <stdint.h>
NSString *totk_rt_dir(void);
NSSet<NSString *> *totk_rt_prime_only(void);   /* local change: effect/UI seed keys, compiled in the background, not loaded at warm-up */
// Returns the variant name whose <dir>/<variant>_b.{metal,json} match the answers, translating when needed.
// stage: NVN shader stage (0 vertex, 1 fragment, 5 compute). answers: {"tex","img","attr","fout"} dictionaries.
NSString *totk_rt_variant(NSString *shader, int stage, const uint8_t *control, const uint8_t *code, NSDictionary *answers, NSArray *local);
// The variant totk_rt_variant would return when the cache index already names one; nil otherwise (never translates).
NSString *totk_rt_variant_cached(NSString *shader, NSDictionary *answers);   // nil on iOS when the variant is not translated yet (request recorded)
#endif
#include <stdint.h>
/* Variants new to the translation cache this run, the variants in the cache (-1 until counted in the background) and
   the CLOCK_UPTIME_RAW time of the last new one (0: none yet). Any pointer may be NULL. */
uint64_t totk_rt_gen(void);        /* local change: changes when a background translation lands */
uint64_t totk_rt_block_ns(void);   /* time callers waited for synchronous translations */
uint64_t totk_rt_pending(void);    /* background translations in flight */
void totk_rt_counts(uint64_t *fresh, int64_t *total, uint64_t *last_new_ns);
