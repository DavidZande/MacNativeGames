/* Deterministic feedback test: snapshot two MRT inputs, overwrite their originals,
   then sample through production encode_args into a separate output. Disabling
   the fix fails without invoking undefined simultaneous attachment sampling. */
int mtl_probe_rt_feedback(void) {
    mtl_init(); int checked=0, old_epoch=atomic_load(&g_arg_epoch_on);
    void *mem=calloc(1,16384); if(!mem)MDIE("feedback probe memory");
    NSError *error=nil;
    NSString *msl=@"#include <metal_stdlib>\nusing namespace metal; struct A { texture2d<float> a [[id(0)]]; texture2d<float> b [[id(1)]]; }; vertex float4 vs(uint i [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],0,1); } fragment float4 fs(constant A &a [[buffer(0)]]) { return float4(a.a.read(uint2(0)).r,a.b.read(uint2(0)).g,a.a.read(uint2(0)).b,1); }";
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:msl options:nil error:&error];if(!lib)MDIE("feedback probe shader: %s",error.localizedDescription.UTF8String);
    NvProg *pr=[NvProg new];NvShader *sh=[NvShader new];pr->sh[1]=sh;sh->stage=1;sh->fn=[lib newFunctionWithName:@"fs"];
    strcpy(sh->key,"probe-rt-feedback");sh->rf.nb=2;
    for(int i=0;i<2;i++)sh->rf.b[i]=(nvn_rbind){.kind=NVN_RB_TEX,.aid=i,.reg=i,.dtt=MTLTextureType2D};
    sh->ae=[sh->fn newArgumentEncoderWithBufferIndex:0];sh->arg_len=sh->ae.encodedLength;
    MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];pd.vertexFunction=[lib newFunctionWithName:@"vs"];pd.fragmentFunction=sh->fn;pd.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> ps=[g_dev newRenderPipelineStateWithDescriptor:pd error:&error];if(!ps)MDIE("feedback probe pipeline");
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
    id<MTLTexture> input[2],output=[g_dev newTextureWithDescriptor:td];res_add(output);
    tex_t t[2]={0};for(int i=0;i<2;i++) {
        td.mipmapLevelCount=2;td.pixelFormat=i?MTLPixelFormatRGBA8Unorm:MTLPixelFormatRGBA8Unorm_sRGB;input[i]=[g_dev newTextureWithDescriptor:td];res_add(input[i]);
        t[i].serial=0xffffd100+i;t[i].mem=(uint8_t *)mem+i*4096;t[i].L.size=4096;
        t[i].d=(nvn_texdesc){.width=8,.height=8,.depth=1,.levels=2,.format=i?0x25:0x38,.target=1,.swizzle_r=2,.swizzle_g=3,.swizzle_b=4,.swizzle_a=5};
        serial_put(t[i].serial,input[i]);
    }
    id<MTLBuffer> rb=probe_buf(512);op_hdr end={OP_END,8};
    for(int epoch=0;epoch<2;epoch++)for(int scenario=0;scenario<3;scenario++)for(int repeat=0;repeat<2;repeat++) {
        atomic_store(&g_arg_epoch_on,epoch);mtl_sub_begin();mtl_exec((const uint8_t *)&end);
        memset(&R,0,sizeof R);R.has_rt=1;R.rt.n=2;
        for(int i=0;i<2;i++) {
            view_t view={.set_levels=1,.base_level=0,.nlevels=1};tex_t alias=t[i];if(scenario==1){alias.serial+=20;alias.mem+=16;alias.L.size-=16;serial_put(alias.serial,input[i]);}mtl_reg_tex(51+i,&alias,scenario==1?&view:NULL);S.tex[1][i]=51+i;
            R.rt.color[i]=&t[i];R.clear_c[i]=1;R.cc[i]=MTLClearColorMake(.25+repeat*.125,.5,.75,1);
        }
        prepare_draw_textures(pr);feedback_prepare(pr); // includes deferred clear materialization
        for(int i=0;i<2;i++){R.clear_c[i]=1;R.cc[i]=MTLClearColorMake(.875,.125,.25,1);}rp_flush();
        if(scenario==2) { // a visibility boundary must expose the newer image
            struct {op_hdr h;int32_t bits,pad;op_hdr end;} barrier={{OP_BARRIER,16},NVN_BARRIER_INVALIDATE_TEXTURE_BIT,0,{OP_END,8}};
            mtl_exec((const uint8_t *)&barrier);feedback_prepare(pr);
        } else feedback_prepare(pr); // reuse immutable image until a boundary
        blit_close();MTL4RenderPassDescriptor *rp=[MTL4RenderPassDescriptor new];rp.colorAttachments[0].texture=output;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;rp.renderTargetWidth=rp.renderTargetHeight=8;
        id<MTL4RenderCommandEncoder> en=enc_r_begin(rp);enc_r_pso(en,ps);enc_r_cull(en,MTLCullModeNone);enc_r_viewport(en,(MTLViewport){0,0,8,8,0,1});enc_r_scissor(en,(MTLScissorRect){0,0,8,8});
        encode_args((id<MTLCommandEncoder>)en,sh,1,0);enc_r_draw(en,MTLPrimitiveTypeTriangle,0,3);enc_r_end(en);
        feedback_reset();if(feedback_texture(51))MDIE("feedback override leaked");checked++;
        enc_b_copy_texture_buffer(blit(),output,0,0,MTLOriginMake(0,0,0),MTLSizeMake(8,8,1),rb,repeat*256,32,256);mtl_sub_commit();if(!repeat)continue;mtl_finish();
        for(int i=0;i<512;i++){int want=scenario==2?(int[]){223,32,64,255}[i%4]:(int[]){i<256?64:96,128,191,255}[i%4];int got=((uint8_t *)rb.contents)[i];if(abs(got-want)>2)MDIE("feedback probe epoch%d scenario%d repeat%d byte%d: %d != %d",epoch,scenario,repeat,i,got,want);checked++;}
    }
    /* Equal guest addresses alone are not evidence of shared Metal storage. */
    mtl_sub_begin();mtl_exec((const uint8_t *)&end);memset(&R,0,sizeof R);R.has_rt=1;R.rt.n=1;R.rt.color[0]=&t[0];
    tex_t unrelated=t[0];unrelated.serial+=10;serial_put(unrelated.serial,input[1]);mtl_reg_tex(51,&unrelated,NULL);S.tex[1][0]=51;S.tex[1][1]=0;
    feedback_prepare(pr);if(g_fb_n)MDIE("feedback false guest alias");checked++;mtl_sub_commit();mtl_finish();
    feedback_reset();feedback_cache_clear();atomic_store(&g_arg_epoch_on,old_epoch);memset(&S,0,sizeof S);memset(&R,0,sizeof R);args_dirty_all();
    res_remove(output);res_remove(rb);free(mem);return checked;
}
