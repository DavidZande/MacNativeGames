/* The name-label shader reads 17 glyph records of 64 bytes from a read-only SSBO. Exercise the production argument
   builder in both vertex and fragment stages, not just the low-level snapshot lookup. */
static int ui_storage_probe(void) {
    mtl_init(); int checked = 0;
    enum { N = 17, W = N * 4, H = 4 };
    typedef struct { float colour[4], rect[4], padding[8]; } glyph;
    const size_t size = 16384; void *mem = NULL;
    if (posix_memalign(&mem, size, size) || !mem) MDIE("glyph storage probe memory");
    memset(mem, 0, size); mtl_pool_add(mem, size);
    glyph *live = mem, *source = (glyph *)((uint8_t *)mem + 4096);
    glyph captured[N], rewritten[N], produced[N];
    for (int i = 0; i < N; i++) {
        captured[i] = (glyph){.colour={(i+1)/32.f,.25,.75,1}, .rect={2.f*(i+.5f)/N-1,0,1.f/N,1}};
        rewritten[i] = captured[i]; produced[i] = captured[i];
        memcpy(rewritten[i].colour, (float[]){.875,.125,.25,1}, 16);
        memcpy(produced[i].colour, (float[]){.125,.75,.5,1}, 16);
    }
    NSError *error = nil;
    NSString *msl = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct G { float4 colour,rect,pad0,pad1; }; struct A { const device G *d [[id(0)]]; };\n"
        "struct O { float4 p [[position]]; uint i [[flat]]; };\n"
        "vertex O vs(uint v [[vertex_id]],uint i [[instance_id]],constant A&a [[buffer(0)]]) {\n"
        "float2 p[6]={float2(-1,-1),float2(1,-1),float2(-1,1),float2(-1,1),float2(1,-1),float2(1,1)};\n"
        "O o; o.p=float4(p[v]*a.d[i].rect.zw+a.d[i].rect.xy,0,1);o.i=i;return o;}\n"
        "fragment float4 fs(O o [[stage_in]],constant A&a [[buffer(0)]]) {return a.d[o.i].colour;}\n";
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:msl options:nil error:&error];
    if (!lib) MDIE("glyph storage probe shader: %s", error.localizedDescription.UTF8String);
    NvShader *sh[2];
    for (int stage = 0; stage < 2; stage++) {
        sh[stage] = [NvShader new]; sh[stage]->stage = stage;
        sh[stage]->fn = [lib newFunctionWithName:stage ? @"fs" : @"vs"];
        snprintf(sh[stage]->key, sizeof sh[stage]->key, "probe-glyph-storage-%d", stage);
        sh[stage]->rf.nb = 1; sh[stage]->rf.b[0] = (nvn_rbind){.kind=NVN_RB_SBUFP,.aid=0,.reg=0};
        sh[stage]->ae = [sh[stage]->fn newArgumentEncoderWithBufferIndex:0]; sh[stage]->arg_len = sh[stage]->ae.encodedLength;
    }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction=sh[0]->fn; pd.fragmentFunction=sh[1]->fn;
    pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!ps) MDIE("glyph storage probe pipeline: %s", error.localizedDescription.UTF8String);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
    td.storageMode=MTLStorageModePrivate; td.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> tx=[g_dev newTextureWithDescriptor:td]; res_add(tx); id<MTLBuffer> rb=probe_buf(W*H*4);
    uint8_t commands[256], *w=commands;
    p_bind_buf sb={0,0,(uint64_t)(uintptr_t)mem,sizeof captured}; w=lp_rec(w,OP_BIND_SSBO,&sb,sizeof sb);
    sb.stage=1;w=lp_rec(w,OP_BIND_SSBO,&sb,sizeof sb);
    p_bind_buf neighbour={0,1,(uint64_t)(uintptr_t)mem+sizeof captured+64,16};w=lp_rec(w,OP_BIND_UBO,&neighbour,sizeof neighbour);lp_end(w);
    uint64_t handle=(uint64_t)(uintptr_t)commands;op_hdr end={OP_END,8};
    int epoch_saved=atomic_load(&g_arg_epoch_on);
    for (int epoch=0;epoch<2;epoch++) for (int scenario=0;scenario<4;scenario++) {
        atomic_store(&g_arg_epoch_on,epoch);memcpy(live,captured,sizeof captured);memcpy(source,produced,sizeof produced);
        void *cap=mtl_sub_capture_ubos(&handle,1);memcpy(live,rewritten,sizeof rewritten);
        if (scenario==3) { // a prior submission writes after capture and finishes before this replay
            mtl_sub_begin();mtl_exec((const uint8_t *)&end);mtl_sub_copy((uintptr_t)source,(uintptr_t)live,sizeof captured);mtl_sub_commit();mtl_finish();
        }
        mtl_sub_begin();mtl_sub_snapshot_ubos_cap(&handle,1,cap);
        if (scenario==2) mtl_sub_copy((uintptr_t)source,(uintptr_t)live,sizeof captured);
        mtl_exec(commands);
        sh[0]->rf.b[0].rw=scenario==1; // writable bindings must stay on the live GPU timeline
        g_pass.serial++;args_dirty_all();
        MTL4RenderPassDescriptor *rp=[MTL4RenderPassDescriptor new];rp.colorAttachments[0].texture=tx;
        rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;
        rp.renderTargetWidth=W;rp.renderTargetHeight=H;
        id<MTL4RenderCommandEncoder> en=enc_r_begin(rp);enc_r_pso(en,ps);enc_r_cull(en,MTLCullModeNone);
        enc_r_viewport(en,(MTLViewport){0,0,W,H,0,1});enc_r_scissor(en,(MTLScissorRect){0,0,W,H});
        encode_args((id<MTLCommandEncoder>)en,sh[0],0,1);encode_args((id<MTLCommandEncoder>)en,sh[1],1,0);
        if (!usnap_find(neighbour.addr,16)) MDIE("glyph storage probe discarded an unrelated neighbouring uniform");checked++;
        enc_r_draw_instanced(en,MTLPrimitiveTypeTriangle,0,6,N,0);enc_r_end(en);
        enc_b_copy_texture_buffer(blit(),tx,0,0,MTLOriginMake(0,0,0),MTLSizeMake(W,H,1),rb,0,W*4,W*H*4);
        mtl_sub_commit();mtl_finish();
        glyph *want=scenario==0?captured:scenario==1?rewritten:produced;
        for (int y=0;y<H;y++) for (int x=0;x<W;x++) for (int k=0;k<4;k++) {
            int expected=(int)lroundf(want[x/4].colour[k]*255),got=((uint8_t *)rb.contents)[(y*W+x)*4+k];
            if (abs(got-expected)>1) MDIE("glyph storage probe epoch %d scenario %d glyph %d channel %d: %u != %u (instance data changed after submit)",epoch,scenario,x/4,k,got,expected);
            checked++;
        }
    }
    atomic_store(&g_arg_epoch_on,epoch_saved);memset(&S,0,sizeof S);args_dirty_all();
    mtl_pool_del(mem);mtl_finish();res_remove(tx);res_remove(rb);free(mem);return checked;
}
