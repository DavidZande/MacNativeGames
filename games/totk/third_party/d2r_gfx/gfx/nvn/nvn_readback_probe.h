/* Hold unrelated GPU work behind an event. Invalidate must return the producer's
   pixels before that later submission retires, including every mip level. */
int mtl_probe_readback_prefetch(void) {
    mtl_init();int checked=0;void *mem=NULL;if(posix_memalign(&mem,16384,65536)||!mem)MDIE("readback probe memory");memset(mem,0,65536);mtl_pool_add(mem,65536);nvn_dirty_add_range(mem,65536);
    pool_t pool={.mem=mem,.size=65536};tex_t t={0};t.serial=0xffffd200;t.mem=mem;t.L.size=65536;
    t.d=(nvn_texdesc){.width=16,.height=16,.depth=1,.levels=5,.format=0x25,.target=1,.swizzle_r=2,.swizzle_g=3,.swizzle_b=4,.swizzle_a=5};
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:YES];td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
    id<MTLTexture> texture=[g_dev newTextureWithDescriptor:td];res_add(texture);serial_put(t.serial,texture);wb_learn(&t);op_hdr end={OP_END,8};
    for(int iteration=0;iteration<5;iteration++) {
        /* Terrain streaming allocates fresh serials with the learned layout. */
        t.serial++;serial_put(t.serial,texture);
        mtl_sub_begin();mtl_exec((const uint8_t *)&end);
        for(unsigned l=0;l<5;l++) {
            memset(&R,0,sizeof R);R.has_rt=1;R.rt.n=1;R.rt.color[0]=&t;R.rt.has_cview[0]=1;R.rt.cview[0]=(view_t){.set_levels=1,.base_level=l,.nlevels=1};R.clear_c[0]=1;R.cc[0]=MTLClearColorMake((l+1)/8.0,(iteration%3+1)/4.0,.75,1);rp_flush();
        }
        wb_note(&t);wb_prefetch_targets(&R.rt,NULL);
        if(iteration==3){wb_prefetch_invalidate_at((uintptr_t)mem,65536,0);if(atomic_load(&g_wb_snapshots))MDIE("readback write invalidation");checked++;wb_prefetch_targets(&R.rt,NULL);}
        if(iteration==4){dm_guest_write_prepare(mem,65536);memset(mem,77,65536);}
        id<MTLSharedEvent> hold=[g_dev newSharedEvent];[g_q4 waitForEvent:hold value:1];uint64_t final=mtl_sub_commit();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,500*NSEC_PER_MSEC),dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{hold.signaledValue=1;});
        mtl_pool_invalidate(&pool,0,65536);
        if(__atomic_load_n(&g_done,__ATOMIC_ACQUIRE)>=final)MDIE("readback waited for unrelated rendering");checked++;
        hold.signaledValue=1;mtl_finish();
        if(iteration==4){for(int i=0;i<65536;i++){if(((uint8_t *)mem)[i]!=77)MDIE("readback overwrote newer CPU bytes");checked++;}continue;}
        for(unsigned l=0;l<5;l++) {
            unsigned w=16>>l;uint8_t pixels[1024];if(nvn_deswizzle(&t.d,l,0,mem,pixels,w*4))MDIE("readback probe deswizzle");
            for(unsigned i=0;i<w*w*4;i++){int want=(int[]){(int)lround((l+1)*255/8.0),(int)lround((iteration%3+1)*255/4.0),191,255}[i%4];if(abs(pixels[i]-want)>1)MDIE("readback mip%u byte%u got%d want%d",l,i,pixels[i],want);checked++;}
        }
    }
    /* Two independent textures share a host page. Decide freshness for both before
       either GPU result is written into that page. */
    tex_t neighbours[2];
    mtl_sub_begin();mtl_exec((const uint8_t *)&end);
    for(int i=0;i<2;i++) {
        neighbours[i]=t;neighbours[i].serial=0xffffd300+i;neighbours[i].mem=(uint8_t *)mem+i*8192;
        neighbours[i].d.width=neighbours[i].d.height=neighbours[i].d.levels=1;nvn_tex_layout(&neighbours[i].d,&neighbours[i].L);
        if(neighbours[i].L.size>8192)MDIE("readback neighbour layout");
        MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
        id<MTLTexture> tx=[g_dev newTextureWithDescriptor:d];res_add(tx);serial_put(neighbours[i].serial,tx);wb_learn(&neighbours[i]);
        memset(&R,0,sizeof R);R.has_rt=1;R.rt.n=1;R.rt.color[0]=&neighbours[i];R.clear_c[0]=1;R.cc[0]=MTLClearColorMake(i==0,i==1,0,1);rp_flush();wb_note(&neighbours[i]);wb_prefetch_targets(&R.rt,NULL);
    }
    mtl_sub_commit();mtl_pool_invalidate(&pool,0,65536);mtl_finish();
    for(int i=0;i<2;i++){uint8_t pixel[4];if(nvn_deswizzle(&neighbours[i].d,0,0,neighbours[i].mem,pixel,4))MDIE("readback neighbour decode");for(int c=0;c<4;c++){int want=c==i||c==3?255:0;if(pixel[c]!=want)MDIE("readback neighbour %d channel %d got%d want%d",i,c,pixel[c],want);checked++;}}
    memset(&R,0,sizeof R);nvn_dirty_remove_range(mem);mtl_pool_del(mem);free(mem);return checked;
}
