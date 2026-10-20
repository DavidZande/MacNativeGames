// Included by nvn_mtl.m (before feedback_prepare): render-target feedback read through framebuffer fetch.
/* TOTK (2026-10-20): the decal / material passes sample the colour attachments they render into (albedo and packed
   normal; docs/shadow-feedback-20261019.md). feedback_prepare corrects that by ending the pass and copying the sampled
   attachment: 19-85 MB of copies and extra pass breaks per frame, +1.2 ms GPU standing / +1.9 ms panning (bench
   20261020-110901). These shaders sample at the fragment's own pixel: the vertex shader writes
   V = ((x + w) / 2, (w - y) / 2, -, w) of its clip position and the fragment shader samples at V.xy / V.w. On an Apple
   GPU that read is framebuffer fetch: the fragment reads its own pixel's current value from tile memory, ordered after
   earlier fragments, with no copy and no pass break.

   fbf_try proves it per (vertex, fragment, register -> attachment) set and, once a variant is compiled in the
   background, gives the draw a fragment function whose samples of those registers read [[color(j)]]. The proof:
   - fragment MSL: every use of the register's texture is .sample(itsSampler, select(UV, float2(0.0), isnan(UV))) with
     UV = float2(M.x * (1.0 / M.w), M.y * (1.0 / M.w)) (or M.x / M.w) of one perspective-interpolated input M,
     texture2d<float>, every temporary assigned once;
   - vertex MSL: the output at M's location is written once each in .x .y .w at function level, .w = the final
     gl_Position.w, .x = (w + x) * 0.5, .y = (w + -y) * 0.5 (or (w - y) * 0.5) of the final gl_Position values, with a
     single return;
   - at the draw: the sampled view is the attachment itself (same Metal storage, level 0 slice 0, one level, same pixel
     format and size, single sample), the attachment is the guest size, and the viewport is the whole target.
   Until a variant and its pipeline are ready the draw takes the copy path, so nothing waits for a compiler.
   TOTK_RT_FEEDBACK_FETCH=0 turns it off (copies as before). */

static int fbf_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_RT_FEEDBACK_FETCH"); v = !(e && !atoi(e)); MLOG("render-target feedback through framebuffer fetch (TOTK_RT_FEEDBACK_FETCH): %s", v ? "on" : "off"); } return v; }

@interface FbfEntry : NSObject { @public _Atomic int state; id<MTLFunction> fn; } @end   /* state: 0 compiling, 1 ready, -1 refused */
@implementation FbfEntry @end
static NSMutableDictionary<NSString *, FbfEntry *> *g_fbf_tab;   // submit thread only
uint64_t g_fbf_draws, g_fbf_refused, g_fbf_ready;   /* g_fbf_fn / g_fbf_fallback / g_fbf_fallbacks: declared before pso_get */

static dispatch_queue_t fbf_queue(void) {
    static dispatch_queue_t q; static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("totk.fbf", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0)); });
    return q;
}

/* ---- MSL facts (spirv-cross output: one temporary per statement, `float _N = expr;`) ---- */
typedef struct { __unsafe_unretained NSDictionary<NSString *, NSString *> *defs; __unsafe_unretained NSSet<NSString *> *reassigned; } fbf_ssa;
static NSDictionary<NSString *, NSString *> *fbf_defs(NSString *s, NSSet<NSString *> **reassigned) {
    NSMutableDictionary *d = [NSMutableDictionary dictionary]; NSMutableSet *re = [NSMutableSet set];
    static NSRegularExpression *rd, *rr; static dispatch_once_t once;
    dispatch_once(&once, ^{
        rd = [NSRegularExpression regularExpressionWithPattern:@"^\\s*(?:float|float2|float3|float4|int|uint|bool)\\s+(_\\d+)\\s*=\\s*([^;]+);" options:NSRegularExpressionAnchorsMatchLines error:nil];
        rr = [NSRegularExpression regularExpressionWithPattern:@"^\\s*(_\\d+)(?:\\.\\w+)?\\s*=[^=]" options:NSRegularExpressionAnchorsMatchLines error:nil]; });
    for (NSTextCheckingResult *m in [rd matchesInString:s options:0 range:NSMakeRange(0, s.length)]) {
        NSString *k = [s substringWithRange:[m rangeAtIndex:1]];
        if (d[k]) [re addObject:k]; else d[k] = [[s substringWithRange:[m rangeAtIndex:2]] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet]; }
    for (NSTextCheckingResult *m in [rr matchesInString:s options:0 range:NSMakeRange(0, s.length)]) [re addObject:[s substringWithRange:[m rangeAtIndex:1]]];
    *reassigned = re; return d;
}
static NSString *fbf_def(fbf_ssa *a, NSString *v) { return v && !(a->reassigned && [a->reassigned containsObject:v]) ? a->defs[v] : nil; }   // the single definition of a temporary
static int fbf_split2(NSString *e, NSString *op, NSString **l, NSString **r) {   // "a OP b" with two plain operands
    NSArray *p = [e componentsSeparatedByString:[NSString stringWithFormat:@" %@ ", op]];
    if (p.count != 2) return 0;
    NSCharacterSet *bad = [NSCharacterSet characterSetWithCharactersInString:@" ()+-*/,?:"];
    for (NSString *x in p) if ([x rangeOfCharacterFromSet:bad].location != NSNotFound) return 0;
    *l = p[0]; *r = p[1]; return 1;
}
/* fragment: the coordinate temporary UV is M.c / M.w for c = x, y; returns M's name or nil */
static NSString *fbf_uv_member(fbf_ssa *a, NSString *uv) {
    NSString *d = fbf_def(a, uv);
    if (![d hasPrefix:@"float2("] || ![d hasSuffix:@")"]) return nil;
    NSArray *xy = [[d substringWithRange:NSMakeRange(7, d.length - 8)] componentsSeparatedByString:@", "];
    if (xy.count != 2) return nil;
    NSString *mem[2] = {nil, nil}, *wmem[2] = {nil, nil}; const char *comp = "xy";
    static NSRegularExpression *rin; static dispatch_once_t once;
    dispatch_once(&once, ^{ rin = [NSRegularExpression regularExpressionWithPattern:@"^in\\.(m_\\d+)\\.([xyzw])$" options:0 error:nil]; });
    NSString *(^input)(NSString *, char) = ^NSString *(NSString *v, char c) {   // v is a temporary holding in.M.c
        NSString *e = fbf_def(a, v); if (!e) return nil;
        NSTextCheckingResult *m = [rin firstMatchInString:e options:0 range:NSMakeRange(0, e.length)];
        return m && [[e substringWithRange:[m rangeAtIndex:2]] characterAtIndex:0] == c ? [e substringWithRange:[m rangeAtIndex:1]] : nil; };
    for (int i = 0; i < 2; i++) {
        NSString *e = fbf_def(a, xy[i]), *l, *r; if (!e) return nil;
        if (fbf_split2(e, @"/", &l, &r)) { mem[i] = input(l, comp[i]); wmem[i] = input(r, 'w'); }
        else if (fbf_split2(e, @"*", &l, &r)) {
            for (int sw = 0; sw < 2 && !mem[i]; sw++) { NSString *p = sw ? r : l, *q = sw ? l : r, *qd = fbf_def(a, q), *ql, *qr;
                if (qd && fbf_split2(qd, @"/", &ql, &qr) && [ql isEqualToString:@"1.0"]) { mem[i] = input(p, comp[i]); wmem[i] = input(qr, 'w'); } } }
        if (!mem[i] || !wmem[i] || ![mem[i] isEqualToString:wmem[i]]) return nil;
    }
    return [mem[0] isEqualToString:mem[1]] ? mem[0] : nil;
}
static NSRange fbf_func_body(NSString *s, NSString *kind) {   // the body of the "<kind> ... main0(" entry
    NSRange f = [s rangeOfString:[NSString stringWithFormat:@"\n%@ ", kind]]; if (f.location == NSNotFound) return f;
    NSRange b = [s rangeOfString:@"\n{" options:0 range:NSMakeRange(f.location, s.length - f.location)]; if (b.location == NSNotFound) return b;
    return NSMakeRange(b.location + 2, s.length - b.location - 2);
}
static int fbf_depth_at(NSString *s, NSRange body, NSUInteger pos) {   // brace depth inside the entry body (0 = function level)
    int d = 0; for (NSUInteger i = body.location; i < pos; i++) { unichar c = [s characterAtIndex:i]; if (c == '{') d++; else if (c == '}') d--; }
    return d;
}
/* vertex: output at location locn = ((w + x) * 0.5, (w + -y) * 0.5, -, w) of the final clip position */
static int fbf_vs_screen(NSString *vs, int locn, char *why, size_t whyn) {
    NSRegularExpression *rm = [NSRegularExpression regularExpressionWithPattern:[NSString stringWithFormat:@"float4\\s+(\\w+)\\s+\\[\\[user\\(locn%d\\)\\]\\];", locn] options:0 error:nil];
    NSTextCheckingResult *m = [rm firstMatchInString:vs options:0 range:NSMakeRange(0, vs.length)];
    if (!m) { snprintf(why, whyn, "vertex output locn%d not a plain float4", locn); return 0; }
    NSString *V = [vs substringWithRange:[m rangeAtIndex:1]];
    NSRange body = fbf_func_body(vs, @"vertex"); if (body.location == NSNotFound) { snprintf(why, whyn, "no vertex entry"); return 0; }
    if ([[vs substringWithRange:body] componentsSeparatedByString:@"return"].count != 2) { snprintf(why, whyn, "vertex shader returns early"); return 0; }
    NSString *val[2][4] = {{nil}};   // [0] = out.V, [1] = out.gl_Position; final value per component
    int nv[4] = {0};
    for (int o = 0; o < 2; o++) {
        NSString *name = o ? @"gl_Position" : V;
        NSRegularExpression *ra = [NSRegularExpression regularExpressionWithPattern:[NSString stringWithFormat:@"out\\.%@(\\.\\w+)?\\s*=\\s*([^;=][^;]*);", name] options:0 error:nil];
        for (NSTextCheckingResult *a in [ra matchesInString:vs options:0 range:NSMakeRange(body.location, vs.length - body.location)]) {
            NSString *sw = [a rangeAtIndex:1].location == NSNotFound ? @"" : [vs substringWithRange:[a rangeAtIndex:1]];
            if (sw.length != 2) { snprintf(why, whyn, "out.%s written as a vector", name.UTF8String); return 0; }
            int c = (int)[@"xyzw" rangeOfString:[sw substringFromIndex:1]].location; if (c < 0 || c > 3) { snprintf(why, whyn, "out.%s%s", name.UTF8String, sw.UTF8String); return 0; }
            if (c == 2) continue;   // z does not take part
            if (fbf_depth_at(vs, body, a.range.location) != 0) { snprintf(why, whyn, "out.%s%s written conditionally", name.UTF8String, sw.UTF8String); return 0; }
            val[o][c] = [[vs substringWithRange:[a rangeAtIndex:2]] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
            if (!o) nv[c]++;
        }
    }
    if (nv[0] != 1 || nv[1] != 1 || nv[3] != 1) { snprintf(why, whyn, "out.%s .x/.y/.w not written exactly once", V.UTF8String); return 0; }
    NSString *X = val[1][0], *Y = val[1][1], *W = val[1][3];
    if (!X || !Y || !W) { snprintf(why, whyn, "gl_Position not written"); return 0; }
    if (![val[0][3] isEqualToString:W]) { snprintf(why, whyn, "out.%s.w is not gl_Position.w", V.UTF8String); return 0; }
    NSSet *re = nil; NSDictionary *defs = fbf_defs(vs, &re); fbf_ssa a = {defs, re};
    for (int c = 0; c < 2; c++) {   // V.c = B * 0.5, B = W + X (c = x) or W + N, N = -Y / B = W - Y (c = y)
        NSString *e = fbf_def(&a, val[0][c]), *l, *r, *B = nil;
        if (e && fbf_split2(e, @"*", &l, &r)) B = [r isEqualToString:@"0.5"] ? l : [l isEqualToString:@"0.5"] ? r : nil;
        NSString *be = fbf_def(&a, B); int ok = 0;
        if (be && fbf_split2(be, @"+", &l, &r)) {
            NSString *other = [l isEqualToString:W] ? r : [r isEqualToString:W] ? l : nil;
            if (c == 0) ok = [other isEqualToString:X];
            else { NSString *nd = fbf_def(&a, other); ok = [nd isEqualToString:[@"-" stringByAppendingString:Y]]; }
        } else if (c == 1 && be && fbf_split2(be, @"-", &l, &r)) ok = [l isEqualToString:W] && [r isEqualToString:Y];
        if (!ok) { snprintf(why, whyn, "out.%s.%c is not the screen position", V.UTF8String, "xy"[c]); return 0; }
    }
    return 1;
}
/* Proof + rewrite (background): fragment source with each listed register's samples read from [[color(att)]] */
static NSString *fbf_rewrite(NvShader *fs, NvShader *vs, const int *regs, const int *atts, int n, char *why, size_t whyn) {
    NSString *src = sh_src(fs), *vsrc = sh_src(vs);
    if (!src || !vsrc) { snprintf(why, whyn, "no MSL source"); return nil; }
    NSSet *re = nil; NSDictionary *defs = fbf_defs(src, &re); fbf_ssa a = {defs, re};
    NSMutableSet<NSNumber *> *used = [NSMutableSet set];
    for (int i = 0; i < n; i++) {
        int aid = -1; for (int b = 0; b < fs->rf.nb; b++) if (fs->rf.b[b].kind == NVN_RB_TEX && fs->rf.b[b].reg == regs[i]) aid = fs->rf.b[b].aid;
        if (aid < 0) { snprintf(why, whyn, "register %d not in the reflection", regs[i]); return nil; }
        NSRegularExpression *rm = [NSRegularExpression regularExpressionWithPattern:[NSString stringWithFormat:@"\\n\\s*(\\S+)\\s+(\\w+)\\s+\\[\\[id\\(%d\\)\\]\\];", aid] options:0 error:nil];
        NSTextCheckingResult *m = [rm firstMatchInString:src options:0 range:NSMakeRange(0, src.length)];
        if (!m || ![[src substringWithRange:[m rangeAtIndex:1]] isEqualToString:@"texture2d<float>"]) { snprintf(why, whyn, "register %d is not a texture2d<float>", regs[i]); return nil; }
        NSString *name = [src substringWithRange:[m rangeAtIndex:2]];
        NSString *pat = [NSString stringWithFormat:@"spvDescriptorSet0\\.%@\\.sample\\(spvDescriptorSet0\\.%@Smplr, select\\((_\\d+), float2\\(0\\.0\\), isnan\\(\\1\\)\\)\\)", name, name];
        NSRegularExpression *rs = [NSRegularExpression regularExpressionWithPattern:pat options:0 error:nil];
        NSArray<NSTextCheckingResult *> *ms = [rs matchesInString:src options:0 range:NSMakeRange(0, src.length)];
        NSUInteger uses = [src componentsSeparatedByString:[NSString stringWithFormat:@"spvDescriptorSet0.%@.", name]].count - 1;
        if (!ms.count || ms.count != uses) { snprintf(why, whyn, "register %d used other than a plain sample (%lu of %lu)", regs[i], (unsigned long)ms.count, (unsigned long)uses); return nil; }
        for (NSTextCheckingResult *u in ms) {
            NSString *M = fbf_uv_member(&a, [src substringWithRange:[u rangeAtIndex:1]]);
            if (!M) { snprintf(why, whyn, "register %d sampled at a coordinate not of the form M.xy / M.w", regs[i]); return nil; }
            NSRegularExpression *rl = [NSRegularExpression regularExpressionWithPattern:[NSString stringWithFormat:@"float4\\s+%@\\s+\\[\\[user\\(locn(\\d+)\\)\\]\\];", M] options:0 error:nil];
            NSTextCheckingResult *l = [rl firstMatchInString:src options:0 range:NSMakeRange(0, src.length)];
            if (!l) { snprintf(why, whyn, "input %s is not a perspective float4", M.UTF8String); return nil; }
            int locn = [[src substringWithRange:[l rangeAtIndex:1]] intValue];
            if (!fbf_vs_screen(vsrc, locn, why, whyn)) return nil;
        }
        src = [rs stringByReplacingMatchesInString:src options:0 range:NSMakeRange(0, src.length) withTemplate:[NSString stringWithFormat:@"totk_fbf_%d", atts[i]]];
        [used addObject:@(atts[i])];
    }
    NSRegularExpression *re2 = [NSRegularExpression regularExpressionWithPattern:[NSString stringWithFormat:@"\\nfragment\\s+\\w+\\s+%@\\(", fs->fn.name] options:0 error:nil];
    NSTextCheckingResult *e = [re2 firstMatchInString:src options:0 range:NSMakeRange(0, src.length)];
    if (!e) { snprintf(why, whyn, "fragment entry not found"); return nil; }
    NSMutableString *params = [NSMutableString string];
    for (NSNumber *j in [used.allObjects sortedArrayUsingSelector:@selector(compare:)]) [params appendFormat:@"float4 totk_fbf_%d [[color(%d)]], ", j.intValue, j.intValue];
    return [src stringByReplacingCharactersInRange:NSMakeRange(NSMaxRange(e.range), 0) withString:params];
}

/* At the draw (submit thread, before any copy): 1 = this draw reads its feedback attachments by framebuffer fetch
   (g_fbf_fn set, no copies needed); 0 = take the copy path. */
static int fbf_try(NvProg *pr) {
    g_fbf_fn = nil;
    if (!fbf_on() || !pr || !R.has_rt || mfx_enabled()) return 0;
    NvShader *vs = pr->sh[0], *fs = pr->sh[1];
    if (!vs || !fs || fs->writes_depth || !sh_has_src(vs) || !sh_has_src(fs)) return 0;
    int regs[16], atts[16], n = 0, ok = 1;
    registry_lock();
    for (int st = 0; st < 2 && ok; st++) { NvShader *sh = pr->sh[st]; if (!sh) continue;
        for (int b = 0; b < sh->rf.nb && ok; b++) { const nvn_rbind *r = &sh->rf.b[b]; if (r->kind != NVN_RB_TEX || r->reg >= 128) continue;
            int tid = (int)(S.tex[st][r->reg] & 0xfffff); if (tid <= 0) continue;
            const treg_t *bound = bound_tex(S.tex[st][r->reg]); if (!bound || !bound->t.mem) continue;
            int alias = 0; for (int a = 0; a < R.rt.n && a < 8; a++) if (R.rt.color[a] && (uintptr_t)R.rt.color[a]->mem < (uintptr_t)bound->t.mem + bound->t.L.size && (uintptr_t)bound->t.mem < (uintptr_t)R.rt.color[a]->mem + R.rt.color[a]->L.size) alias = 1;
            if (!alias) continue;
            id<MTLTexture> src = tex_by_id(tid); if (!src) continue;
            id<MTLTexture> root = src; while (root.parentTexture) root = root.parentTexture;
            int att = -1; id<MTLTexture> at = nil;
            for (int a = 0; a < R.rt.n && a < 8 && att < 0; a++) if (R.rt.color[a]) { id<MTLTexture> t = attachment_texture(R.rt.color[a], &R.rt.cview[a], R.rt.has_cview[a]), tr = t; while (tr.parentTexture) tr = tr.parentTexture; if (tr == root) { att = a; at = t; } }
            if (att < 0) continue;   // guest memory overlaps but the Metal storage differs: not feedback (as feedback_prepare)
            uint64_t s_; int lv, sl; rt_sub(&R.rt, att, &s_, &lv, &sl);
            const tex_t *ct = R.rt.color[att];
            if (st != 1 || lv || sl || src.sampleCount != 1 || at.sampleCount != 1 || src.textureType != MTLTextureType2D || src.mipmapLevelCount != 1 ||
                (src.parentTexture && (src.parentRelativeLevel || src.parentRelativeSlice)) || src.pixelFormat != at.pixelFormat ||
                src.width != at.width || src.height != at.height || at.width != MAX(ct->d.width, 1u) || at.height != MAX(ct->d.height, 1u) ||
                (S.has_vp && !(S.vp[0][0] == 0 && S.vp[0][1] == 0 && S.vp[0][2] == (float)at.width && S.vp[0][3] == (float)at.height)) || n == 16) ok = 0;
            else { regs[n] = r->reg; atts[n++] = att; }
        } }
    registry_unlock();
    if (!ok || !n) return 0;
    NSMutableString *key = [NSMutableString stringWithFormat:@"%s|%s", fs->key, vs->key];
    for (int i = 0; i < n; i++) [key appendFormat:@"|%d:%d", regs[i], atts[i]];
    if (!g_fbf_tab) g_fbf_tab = [NSMutableDictionary dictionary];
    FbfEntry *en = g_fbf_tab[key];
    if (!en) {
        en = [FbfEntry new]; g_fbf_tab[key] = en;
        NvShader *fs2 = fs, *vs2 = vs; NSString *k2 = [key copy];
        struct { int r[16], a[16], n; } p; memcpy(p.r, regs, sizeof p.r); memcpy(p.a, atts, sizeof p.a); p.n = n;   // blocks capture structs, not arrays
        dispatch_async(fbf_queue(), ^{ @autoreleasepool {
            char why[160] = "";
            NSString *msl = fbf_rewrite(fs2, vs2, p.r, p.a, p.n, why, sizeof why);
            id<MTLFunction> f = nil;
            if (msl) { NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:msl_salted(msl) options:totk_precise_opts() error:&e];
                f = lib ? [lib newFunctionWithName:fs2->fn.name] : nil;
                if (!f) snprintf(why, sizeof why, "variant does not compile: %s", e.localizedDescription.UTF8String); }
            en->fn = f; atomic_store(&en->state, f ? 1 : -1);
            if (f) { g_fbf_ready++; MLOG("[fb-fetch] %s: attachment reads by framebuffer fetch ready", k2.UTF8String); }
            else { g_fbf_refused++; MLOG("[fb-fetch] %s: copies kept (%s)", k2.UTF8String, why); }
        } });
    }
    if (atomic_load(&en->state) != 1) return 0;
    g_fbf_fn = en->fn;
    if (++g_fbf_draws % 20000 == 1) MLOG("[fb-fetch] %llu draws read their attachments by framebuffer fetch (%llu variants ready, %llu refused, %llu pipeline fallbacks)",
                                         (unsigned long long)g_fbf_draws, (unsigned long long)g_fbf_ready, (unsigned long long)g_fbf_refused, (unsigned long long)g_fbf_fallbacks);
    return 1;
}
