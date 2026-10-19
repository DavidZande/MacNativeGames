// Compute dispatches cannot be skipped while compiling: their outputs may be
// addresses consumed by subsequent GPU work. Discover pipelines from reflection,
// including variants and effect/UI shaders absent from the played PSO lists.
#pragma once
#import <Foundation/Foundation.h>
#include "dmsh_chain.h"

static NSString *totk_compute_warm_manifest(NSString *cache, NSArray<NSString *> *files) {
    NSMutableString *rows = [NSMutableString string];
    for (NSString *file in files) @autoreleasepool {
        if (![file hasSuffix:@"_b.metal"]) continue;
        NSString *key = [file stringByDeletingPathExtension];
        NSData *data = [NSData dataWithContentsOfFile:[cache stringByAppendingPathComponent:[key stringByAppendingPathExtension:@"json"]]];
        id reflection = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        if (![reflection isKindOfClass:NSDictionary.class] ||
            ![reflection[@"chain"] isEqual:@DMSH_CHAIN] || ![reflection[@"stage"] isEqual:@"cs"]) continue;
        [rows appendFormat:@"C\t%@\n", key];
    }
    return rows;
}
