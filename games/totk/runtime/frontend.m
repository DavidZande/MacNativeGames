/* Native macOS window and VI service adapter. No guest frame is synthesized. */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#include <stdatomic.h>
#include <math.h>
#include "runtime.h"
#include "game_version.h"
#include <sys/clonefile.h>
static NSWindow *window;
uint64_t dm_nvn_frame_count(void);   /* gfx/nvn/hle_nvn.c: game frames presented so far */
static CAMetalLayer *metalLayer;
static NSView *bootOverlay;
static NSTextField *bootStage,*bootDetail;
static NSProgressIndicator *bootProgress;
static NSView *shortcutHelp;
static uint64_t bootStartNS;
static _Atomic uint64_t presentedFrames;
static _Atomic int warmPending;
static CVDisplayLinkRef displayLink;
static _Atomic int focused;
static pthread_mutex_t vsyncLock=PTHREAD_MUTEX_INITIALIZER;
static void *vsyncEvents[16];
static unsigned vsyncCount;
void hle_bind_system_event(void *,int);
void hle_signal_system_event(void *);
static void settings_install(void);static void settings_apply_live(void);static void front_take_screenshot(void);   /* local change: the settings window (end of this file) */
void *front_metal_layer(void){return (__bridge void *)metalLayer;}
void front_frame_presented(void){
 if(atomic_fetch_add_explicit(&presentedFrames,1,memory_order_relaxed)==0){
  fprintf(stderr,"[totk-boot] %.3f first presented frame\n",(runtime_now_ns()-bootStartNS)/1e9);
  dispatch_async(dispatch_get_main_queue(), ^{settings_apply_live();});
  dispatch_async(dispatch_get_main_queue(), ^{if(!atomic_load(&warmPending))bootOverlay.hidden=YES;});
 }
}
/* Local change: English text on screen; the log lines keep the original stage names. */
static NSString *ui_en(const char *zh){
 static NSDictionary<NSString *,NSString *> *m;static dispatch_once_t once;dispatch_once(&once,^{m=@{
  @"正在启动":@"Starting",@"正在处理，请稍候":@"Working, please wait",
  @"读取资源索引":@"Reading the resource index",@"校验游戏资源":@"Verifying game data",@"游戏资源校验记录有效":@"Game data verification record is valid",
  @"准备游戏资源":@"Preparing game data",@"检查分辨率补丁并挂载只读游戏资源":@"Checking the resolution patch and mounting the read-only game data",
  @"启动游戏运行环境":@"Starting the game runtime",@"连接游戏代码与 macOS 系统服务":@"Connecting the game code to macOS system services",
  @"初始化游戏模块":@"Initializing game modules",
  @"创建 Metal 图形设备":@"Creating the Metal graphics device",@"准备 GPU、命令队列和图形资源":@"Preparing the GPU, command queues and graphics resources",
  @"准备着色器缓存":@"Preparing the shader cache",@"检查本机缓存并读取着色器清单":@"Checking the local cache and reading the shader list",
  @"检查着色器缓存":@"Checking the shader cache",@"校验 App 附带的着色器，并按需导入本机缓存":@"Verifying the bundled shaders and importing them into the local cache",
  @"着色器缓存已就绪":@"Shader cache ready",@"没有内置着色器种子，继续初始化图形":@"No bundled shader seed; continuing graphics setup",
  @"本机缓存已存在，无需重新导入；开始预热":@"Local cache already present; starting warm-up"};});
 if(!zh)return @"Starting";
 NSString *s=[NSString stringWithUTF8String:zh];if(!s)return @"";
 NSString *t=m[s];if(t)return t;
 unsigned a=0,b=0;char name[32];unsigned long long k=0;
 if(sscanf(zh,"运行 %31s 游戏模块初始化代码（%u/%u）",name,&a,&b)==3)return [NSString stringWithFormat:@"Running the %s module's initialization code (%u/%u)",name,a,b];
 if(sscanf(zh,"已导入 %llu 个预置文件",&k)==1)return [NSString stringWithFormat:@"Imported %llu prepared files; starting warm-up",k];
 return s;
}
void runtime_boot_progress(const char *stage,uint64_t done,uint64_t total){
 static const char *last;
 if(stage!=last){fprintf(stderr,"[totk-boot] %.3f %s\n",bootStartNS?(runtime_now_ns()-bootStartNS)/1e9:0,stage?stage:"startup");last=stage;}
 NSString *s=ui_en(stage);
 dispatch_async(dispatch_get_main_queue(), ^{
  if(!bootOverlay||bootOverlay.hidden)return;
  bootStage.stringValue=s;
  if(total){bootDetail.stringValue=[NSString stringWithFormat:@"%.1f / %.1f GB",done/1e9,total/1e9];
   bootProgress.indeterminate=NO;bootProgress.doubleValue=(double)done/(double)total*100.0;}
  else {bootDetail.stringValue=@"Working, please wait";bootProgress.indeterminate=YES;[bootProgress startAnimation:nil];}
 });
}
void runtime_boot_status(const char *stage,const char *detail){
 if(stage&&!strcmp(stage,"Compiling shaders")){NSString *d=@(detail?detail:"");dispatch_async(dispatch_get_main_queue(),^{if(!bootOverlay||bootOverlay.hidden)return;bootStage.stringValue=@"Compiling shaders";bootDetail.stringValue=d;});return;}   /* frequent: not logged (local change) */
 if(bootStartNS)fprintf(stderr,"[totk-boot] %.3f %s — %s\n",(runtime_now_ns()-bootStartNS)/1e9,stage?stage:"正在启动",detail?detail:"");
 NSString *s=ui_en(stage);
 NSString *d=detail?ui_en(detail):@"Working, please wait";
 dispatch_async(dispatch_get_main_queue(), ^{
  if(!bootOverlay||bootOverlay.hidden)return;
  bootStage.stringValue=s;bootDetail.stringValue=d;
  bootProgress.indeterminate=YES;[bootProgress startAnimation:nil];
 });
}
void runtime_warm_progress(uint64_t done,uint64_t total,uint64_t shaders,uint64_t shader_total,uint64_t pipelines,uint64_t pipeline_total){
 atomic_store(&warmPending,1);
 dispatch_async(dispatch_get_main_queue(), ^{
  if(!bootOverlay||bootOverlay.hidden)return;
  double percent=total ? fmin(100.0,(double)done/(double)total*100.0) : 100.0;
  bootStage.stringValue=[NSString stringWithFormat:@"Warming up shaders and pipelines %.0f%%",floor(percent)];
  bootDetail.stringValue=[NSString stringWithFormat:@"%llu/%llu items · shaders %llu/%llu · pipelines %llu/%llu",
      (unsigned long long)done,(unsigned long long)total,(unsigned long long)shaders,(unsigned long long)shader_total,
      (unsigned long long)pipelines,(unsigned long long)pipeline_total];
  bootProgress.indeterminate=NO;
  bootProgress.doubleValue=percent;
 });
}
void runtime_warm_done(void){
 atomic_store(&warmPending,0);
 dispatch_async(dispatch_get_main_queue(), ^{if(atomic_load(&presentedFrames))bootOverlay.hidden=YES;});
}
uint64_t front_game_res(int docked){(void)docked;return (720ULL<<32)|1280;}
/* TOTK_ASSUME_FOCUS=1: unattended test runs. macOS refuses to activate an app launched from the background while the user is working in
   another one; the window then never becomes key, GetCurrentFocusState stays 0 and the game waits before its first frame. In this mode the
   game is told it is focused, and the window is shown without taking the keyboard from the user (scripted input does not need it). */
static int assume_focus(void){static int v=-1;if(v<0){const char *e=getenv("TOTK_ASSUME_FOCUS");v=e&&atoi(e);}return v;}
/* Keep the guest active when the macOS window loses key status. Native input is still cleared
   in windowDidResignKey so a held key cannot remain stuck while another app has focus. */
int runtime_front_focus(void){return 1;}
static CVReturn vsync_callback(CVDisplayLinkRef link,const CVTimeStamp *now,const CVTimeStamp *output,CVOptionFlags flags,CVOptionFlags *out,void *ctx){
 (void)link;(void)now;(void)output;(void)flags;(void)out;(void)ctx;
 pthread_mutex_lock(&vsyncLock);for(unsigned i=0;i<vsyncCount;i++)hle_signal_system_event(vsyncEvents[i]);pthread_mutex_unlock(&vsyncLock);return kCVReturnSuccess;
}
@interface TOTKInputView:NSView
@end
@implementation TOTKInputView
-(BOOL)acceptsFirstResponder{return YES;}
-(void)keyDown:(NSEvent *)e{if(e.keyCode==111){if(!e.isARepeat)front_take_screenshot();return;}runtime_input_key(e.keyCode,1);}   /* F12: screenshot (local change) */
-(void)keyUp:(NSEvent *)e{runtime_input_key(e.keyCode,0);}
-(void)flagsChanged:(NSEvent *)e{if(e.keyCode==56)runtime_input_key(56,!!(e.modifierFlags&NSEventModifierFlagShift));if(e.keyCode==58)runtime_input_key(58,!!(e.modifierFlags&NSEventModifierFlagOption));}
@end
@interface TOTKShortcutHelp:NSView
@property(strong) NSTextField *label;
-(void)position;
@end
@implementation TOTKShortcutHelp
-(instancetype)initWithFrame:(NSRect)frame{if((self=[super initWithFrame:frame])){self.wantsLayer=YES;self.layer.backgroundColor=[NSColor colorWithCalibratedWhite:0.04 alpha:0.86].CGColor;self.layer.cornerRadius=14;self.layer.borderWidth=1;self.layer.borderColor=[NSColor colorWithCalibratedWhite:1 alpha:0.2].CGColor;
 self.label=[NSTextField labelWithString:@"Shortcuts\n1  Show / hide all hints\n2  Frame interpolation: off → 30+30 → half refresh rate → unlimited\n3  Picture: MetalFX 2K → native 2K → off"];self.label.font=[NSFont systemFontOfSize:17 weight:NSFontWeightMedium];self.label.textColor=NSColor.whiteColor;self.label.alignment=NSTextAlignmentCenter;self.label.maximumNumberOfLines=0;self.label.lineBreakMode=NSLineBreakByWordWrapping;self.label.frame=NSInsetRect(self.bounds,20,18);self.label.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;[self addSubview:self.label];}return self;}
-(NSView *)hitTest:(NSPoint)p{(void)p;return nil;}
-(void)viewDidMoveToSuperview{[super viewDidMoveToSuperview];[self position];}
-(void)layout{[super layout];[self position];}
-(void)position{if(!self.superview)return;CGFloat w=MIN(560,MAX(280,self.superview.bounds.size.width-40)),h=140;NSRect f=NSMakeRect(floor((self.superview.bounds.size.width-w)/2),floor((self.superview.bounds.size.height-h)/2),w,h);if(!NSEqualRects(self.frame,f))self.frame=f;self.label.frame=NSInsetRect(self.bounds,20,18);}
@end
void runtime_shortcut_help_toggle(void){(void)fps_overlay_toggle_hints();}   /* local change: key 1 toggles the overlays; the shortcut box is gone (the settings window lists the keys) */
/* The drawable follows the window's pixel size; the present pass scales the game picture (1920x1080 docked) into it and
   keeps 16:9 (black bars on other shapes), so a larger window or full screen shows the game at its own resolution instead
   of a fixed small drawable that the compositor stretches. */
/* TOTK_DRAWABLE_SCALE=<f> (test scripts only): the drawable is that many times the window's pixels and the window server
   scales it down for the screen. It lets a test exercise the 2K output path (a 2560x1440 picture) on a screen smaller than
   that (2026-10-17: the Mac reported a 1920x1080 display); the desktop launcher never sets it. */
static CGFloat drawable_scale(void){static CGFloat f=-1;if(f<0){const char *e=getenv("TOTK_DRAWABLE_SCALE");f=e?atof(e):1;if(f<0.25||f>4)f=1;}return f;}
static void front_sync_drawable(void){
 NSView *v=window.contentView;if(!v||!metalLayer)return;
 CGFloat s=window.backingScaleFactor>0?window.backingScaleFactor:1,ts=drawable_scale();CGSize px=CGSizeMake(round(v.bounds.size.width*s*ts),round(v.bounds.size.height*s*ts));
 if(px.width<1||px.height<1||CGSizeEqualToSize(metalLayer.drawableSize,px))return;
 metalLayer.contentsScale=s;metalLayer.drawableSize=px;fprintf(stderr,"[totk] window drawable %.0fx%.0f (scale %.0f)\n",px.width,px.height,s);
}
/* Local change: rolling save backups. An APFS clone (clonefile: instant, shares the data) of the profile folder goes to
   <data>/backups/<yyyyMMdd-HHmmss>-<reason>; the newest 10 are kept. Shared with runtime/frontend.m (same rules). */
static void profile_backup(NSString *profile, NSString *reason) {
    NSFileManager *fm = NSFileManager.defaultManager; BOOL dir = NO;
    if (![fm fileExistsAtPath:profile isDirectory:&dir] || !dir) return;
    NSString *backups = [[profile stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"backups"];
    [fm createDirectoryAtPath:backups withIntermediateDirectories:YES attributes:nil error:nil];
    NSDateFormatter *f = [NSDateFormatter new]; f.dateFormat = @"yyyyMMdd-HHmmss"; f.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    NSString *dst = [backups stringByAppendingPathComponent:[NSString stringWithFormat:@"%@-%@", [f stringFromDate:NSDate.date], reason]];
    if (clonefile(profile.fileSystemRepresentation, dst.fileSystemRepresentation, 0) != 0) { fprintf(stderr, "[backup] clone of %s failed: %s\n", profile.UTF8String, strerror(errno)); return; }
    NSArray<NSString *> *all = [[fm contentsOfDirectoryAtPath:backups error:nil] sortedArrayUsingSelector:@selector(compare:)];
    NSMutableArray<NSString *> *mine = [NSMutableArray array];
    for (NSString *n in all) if (n.length > 15 && [n characterAtIndex:8] == '-' && ![n hasPrefix:@"."]) [mine addObject:n];
    for (NSUInteger i = 0; i + 10 < mine.count; i++) [fm removeItemAtPath:[backups stringByAppendingPathComponent:mine[i]] error:nil];
    fprintf(stderr, "[backup] %s\n", dst.UTF8String);
}
static NSString *profile_dir(void){const char *e=getenv("TOTK_PROFILE_DIR");return e&&*e?@(e):nil;}
static int player_game(void){const char *o=getenv("TOTK_SLOT_OWNER");return !o||!*o||!strcmp(o,"user");}   /* agents' tests run on save copies */
static void exit_backup(void){NSString *p=profile_dir();if(p&&player_game())profile_backup(p,@"exit");}
@interface TOTKWindowDelegate:NSObject<NSWindowDelegate>
@end
@implementation TOTKWindowDelegate
-(void)windowDidBecomeKey:(NSNotification *)n{(void)n;atomic_store(&focused,1);runtime_oe_push_message(15);fprintf(stderr,"[totk] window gained focus\n");}
-(void)windowDidResignKey:(NSNotification *)n{(void)n;atomic_store(&focused,0);runtime_input_clear();runtime_oe_push_message(15);fprintf(stderr,"[totk] window lost focus\n");}
/* The process ends with the window. Save writes go straight to the files (runtime/fs_host.c), so a close in the middle of a
   save waits (up to 15 s, the window already gone) until the game has written and committed it (runtime_fs_save_busy). */
-(void)windowWillClose:(NSNotification *)n{(void)n;
 if(!runtime_fs_save_busy()){fprintf(stderr,"[totk] window closed; graceful guest shutdown pending\n");exit_backup();_exit(0);}
 fprintf(stderr,"[totk] window closed while the game is saving: quitting when the save is done\n");
 dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
  uint64_t t0=runtime_now_ns();while(runtime_fs_save_busy()&&runtime_now_ns()-t0<15000000000ull)usleep(20000);
  fprintf(stderr,"[totk] save %s after %.2f s; quitting\n",runtime_fs_save_busy()?"still in progress (gave up)":"done",(runtime_now_ns()-t0)/1e9);if(!runtime_fs_save_busy())exit_backup();_exit(0);});
}
-(void)windowDidResize:(NSNotification *)n{(void)n;front_sync_drawable();}
/* Whether any part of the window can be seen (TOTK, 2026-10-16 night): macOS stops the display link's callbacks for a
   fully covered window, so presents stop and per-picture measurements of that stretch are void; a test session's log
   says when (the user working in other apps covered the test windows of the frame interpolation tests). */
static _Atomic int windowVisible=-1;   /* -1 not known yet; read by the present path's stall lines (runtime_front_window_visible) */
static void note_occlusion(const char *when){int v=(window.occlusionState&NSWindowOcclusionStateVisible)!=0;atomic_store(&windowVisible,v);
 fprintf(stderr,"[totk] window %s%s (frame %llu)\n",v?"visible":"covered (or on another Space): no presents until it is visible again",when,(unsigned long long)dm_nvn_frame_count());}
-(void)windowDidChangeOcclusionState:(NSNotification *)n{(void)n;note_occlusion("");}
-(void)windowDidChangeBackingProperties:(NSNotification *)n{(void)n;front_sync_drawable();}
@end
static TOTKWindowDelegate *windowDelegate;
int runtime_front_window_visible(void){return atomic_load(&windowVisible);}
void runtime_front_request_close(void){dispatch_async(dispatch_get_main_queue(), ^{[window performClose:nil];});}
static void *guest_main(void *callback){@autoreleasepool{((void(*)(void))callback)();}return NULL;}
void runtime_front_run(void(*callback)(void)){
 @autoreleasepool{
  bootStartNS=runtime_now_ns();
  [NSApplication sharedApplication];[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  runtime_gamepad_start();
  /* Default window: the docked output size, 1920x1080, scaled down (16:9 kept) when the screen's visible area is smaller. */
  /* TOTK_WINDOW_SIZE=<w>x<h> (test scripts only, e.g. the 2K output test; the desktop launcher never sets it): another starting size. */
  NSRect vis=NSScreen.mainScreen?NSScreen.mainScreen.visibleFrame:NSMakeRect(0,0,1920,1080+28);CGFloat ww=1920,wh=1080;
  {const char *e=getenv("TOTK_WINDOW_SIZE");int w=0,h=0;if(e&&sscanf(e,"%dx%d",&w,&h)==2&&w>=320&&h>=180&&w<=8192&&h<=8192){ww=w;wh=h;}}
  CGFloat k=fmin(1,fmin(vis.size.width/ww,(vis.size.height-28)/wh));ww=floor(ww*k);wh=floor(wh*k);
  window=[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,ww,wh) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
  window.releasedWhenClosed=NO;   /* a close that waits for a save (windowWillClose) must not free the window the static pointer still holds */
  /* An agent's test game (TOTK_SLOT_OWNER other than user) says so in its title: it plays a save copy and its script closes it. */
  const char *owner=getenv("TOTK_SLOT_OWNER"),*purpose=getenv("TOTK_SLOT_PURPOSE");
  window.title=owner&&*owner&&strcmp(owner,"user")?[NSString stringWithFormat:@"Test game (%s) Ver. %s: %s — a save copy; nothing is saved to your saves, and the script closes it",owner,TOTK_GAME_VERSION,purpose&&*purpose?purpose:"agent test"]:[NSString stringWithFormat:@"Tears of the Kingdom  Mac — Ver. %s",TOTK_GAME_VERSION];
  windowDelegate=[TOTKWindowDelegate new];window.delegate=windowDelegate;
  window.collectionBehavior|=NSWindowCollectionBehaviorFullScreenPrimary;
  window.contentView=[[TOTKInputView alloc] initWithFrame:NSMakeRect(0,0,ww,wh)];
  metalLayer=[CAMetalLayer layer];metalLayer.device=MTLCreateSystemDefaultDevice();metalLayer.pixelFormat=MTLPixelFormatBGRA8Unorm;metalLayer.drawableSize=CGSizeMake(ww,wh);
  metalLayer.framebufferOnly=NO;window.contentView.wantsLayer=YES;window.contentView.layer=metalLayer;
  bootOverlay=[[NSView alloc] initWithFrame:window.contentView.bounds];bootOverlay.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
  bootOverlay.wantsLayer=YES;bootOverlay.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.035 green:0.075 blue:0.09 alpha:1].CGColor;
  bootStage=[NSTextField labelWithString:@"Starting Hyrule"];
  bootStage.font=[NSFont systemFontOfSize:25 weight:NSFontWeightSemibold];bootStage.textColor=NSColor.whiteColor;
  bootStage.alignment=NSTextAlignmentCenter;bootStage.frame=NSMakeRect(0,wh/2+20,ww,40);bootStage.autoresizingMask=NSViewWidthSizable|NSViewMinYMargin|NSViewMaxYMargin;
  bootDetail=[NSTextField labelWithString:@"Preparing the game environment"];
  bootDetail.font=[NSFont systemFontOfSize:14];bootDetail.textColor=[NSColor colorWithCalibratedWhite:0.72 alpha:1];
  bootDetail.alignment=NSTextAlignmentCenter;bootDetail.frame=NSMakeRect(0,wh/2-20,ww,28);bootDetail.autoresizingMask=NSViewWidthSizable|NSViewMinYMargin|NSViewMaxYMargin;
  bootProgress=[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(ww/2-140,wh/2-55,280,10)];
  bootProgress.minValue=0;bootProgress.maxValue=100;bootProgress.indeterminate=YES;bootProgress.autoresizingMask=NSViewMinXMargin|NSViewMaxXMargin|NSViewMinYMargin|NSViewMaxYMargin;
  [bootOverlay addSubview:bootStage];[bootOverlay addSubview:bootDetail];[bootOverlay addSubview:bootProgress];
  [window.contentView addSubview:bootOverlay];[bootProgress startAnimation:nil];
  shortcutHelp=[[TOTKShortcutHelp alloc] initWithFrame:NSZeroRect];shortcutHelp.autoresizingMask=NSViewMinXMargin|NSViewMaxXMargin|NSViewMinYMargin|NSViewMaxYMargin;
  /* Keep discovery/warm-up visible; the help appears after the opaque boot
     overlay is hidden. Key 1 still controls its requested visibility. */
  [window.contentView addSubview:shortcutHelp positioned:NSWindowBelow relativeTo:bootOverlay];shortcutHelp.hidden=YES;
  [(TOTKShortcutHelp *)shortcutHelp position];
  /* The FPS counter is drawn into the game picture's top-right corner (with the on-screen size) by the present encoder (gfx/nvn/fps_overlay.c). */
  [window makeFirstResponder:window.contentView];[window center];front_sync_drawable();
  settings_install();
  if(assume_focus()){[window orderFrontRegardless];}else{[window makeKeyAndOrderFront:nil];[NSApp activateIgnoringOtherApps:YES];}   /* in front of other apps' windows, keyboard left alone */
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,3*NSEC_PER_SEC),dispatch_get_main_queue(),^{note_occlusion(" 3 s after opening");});
  atomic_store(&focused,1);
  if(CVDisplayLinkCreateWithActiveCGDisplays(&displayLink)||CVDisplayLinkSetOutputCallback(displayLink,vsync_callback,NULL)||CVDisplayLinkStart(displayLink))runtime_fail("native display link initialization");
  pthread_t guest;pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,8*1024*1024);int err=pthread_create(&guest,&attr,guest_main,(void *)callback);pthread_attr_destroy(&attr);if(err)runtime_fail("native game thread creation");
  [NSApp run];runtime_fail("native event loop returned unexpectedly");
 }
}
/* Text entry for the game (nn::swkbd::ShowKeyboard, runtime/swkbd_host.c; on the Switch a keyboard applet takes the screen).
   User, 2026-10-17: no on-screen keyboard, just the Mac keyboard. A bar at the bottom of the game window shows the game's
   prompt and one text field (input methods work); Return confirms, Escape cancels. Characters the game disallows and text
   beyond its length limit are removed as they are typed (the caller checks again). The calling guest thread waits on a
   semaphore; the game's keys are cleared while the bar has the keyboard. */
@interface TOTKTextBar:NSView<NSTextFieldDelegate>
@property(strong) NSTextField *field;
@property int maxUnits,minUnits,digitsOnly;
@property uint32_t disabled;
@property(copy) void(^done)(NSString *text);   /* nil: cancelled */
-(void)refilter;
@end
@implementation TOTKTextBar
-(BOOL)allowed:(unichar)c{
 BOOL digit=c>='0'&&c<='9';uint32_t d=self.disabled;
 if(self.digitsOnly&&!digit)return NO;
 if(c<0x20||c==0x7f)return NO;
 if((d&1u<<1&&c==' ')||(d&1u<<2&&c=='@')||(d&1u<<3&&c=='%')||(d&1u<<4&&c=='/')||(d&1u<<5&&c=='\\')||(d&1u<<6&&digit))return NO;
 return YES;
}
-(void)controlTextDidChange:(NSNotification *)n{(void)n;[self refilter];}
-(void)refilter{
 NSText *ed=self.field.currentEditor;if([ed isKindOfClass:NSTextView.class]&&((NSTextView *)ed).hasMarkedText)return;   /* still composing */
 NSString *s=self.field.stringValue;NSMutableString *t=[NSMutableString string];
 [s enumerateSubstringsInRange:NSMakeRange(0,s.length) options:NSStringEnumerationByComposedCharacterSequences usingBlock:^(NSString *sub,NSRange r,NSRange er,BOOL *stop){
  (void)r;(void)er;
  if(sub.length==1&&![self allowed:[sub characterAtIndex:0]])return;
  if(self.maxUnits>0&&t.length+sub.length>(NSUInteger)self.maxUnits){*stop=YES;return;}
  [t appendString:sub];}];
 if(![t isEqualToString:s]){self.field.stringValue=t;NSBeep();}
}
-(void)finish:(NSString *)text{void(^d)(NSString *)=self.done;self.done=nil;if(d)d(text);}
-(BOOL)control:(NSControl *)c textView:(NSTextView *)tv doCommandBySelector:(SEL)sel{
 (void)c;(void)tv;
 if(sel==@selector(insertNewline:)){[self refilter];NSString *s=self.field.stringValue;
  if((NSInteger)s.length<self.minUnits||!s.length){NSBeep();return YES;}[self finish:s];return YES;}
 if(sel==@selector(cancelOperation:)){[self finish:nil];return YES;}
 return NO;
}
@end
static __weak TOTKTextBar *openBar;   /* main thread only */
void runtime_front_text_test(const char *op,const char *text){
 NSString *o=[NSString stringWithUTF8String:op],*t=[NSString stringWithUTF8String:text?text:""]?:@"";
 dispatch_async(dispatch_get_main_queue(), ^{
  TOTKTextBar *bar=openBar;if(!bar){fprintf(stderr,"[totk] keyboard test: no text entry bar is open\n");return;}
  if([o isEqualToString:@"type"]){bar.field.stringValue=[bar.field.stringValue stringByAppendingString:t];[bar refilter];}
  else if([o isEqualToString:@"enter"])[bar control:bar.field textView:(NSTextView *)bar.field.currentEditor doCommandBySelector:@selector(insertNewline:)];
  else if([o isEqualToString:@"shot"]){   /* the bar as drawn (window screenshots of the game picture do not include AppKit views) */
   NSBitmapImageRep *rep=[bar bitmapImageRepForCachingDisplayInRect:bar.bounds];[bar cacheDisplayInRect:bar.bounds toBitmapImageRep:rep];
   NSData *png=[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];BOOL ok=[png writeToFile:t atomically:YES];
   fprintf(stderr,"[totk] keyboard test: bar picture %s %s\n",t.UTF8String,ok?"written":"FAILED");return;}
  else if([o isEqualToString:@"esc"])[bar control:bar.field textView:(NSTextView *)bar.field.currentEditor doCommandBySelector:@selector(cancelOperation:)];
  fprintf(stderr,"[totk] keyboard test: %s -> field \"%s\"\n",o.UTF8String,bar.field.stringValue.UTF8String);
 });
}
int runtime_front_text_input(const RuntimeTextRequest *req,char *out,size_t out_size){
 if(!window||!req||!out||!out_size){fprintf(stderr,"[totk] keyboard: no game window for text entry; cancelled\n");return 0;}
 NSString *prompt=[NSString stringWithUTF8String:req->prompt?req->prompt:""]?:@"",*initial=[NSString stringWithUTF8String:req->initial?req->initial:""]?:@"";
 int maxU=req->max_units,minU=req->min_units,digits=req->digits_only,secure=req->secure;uint32_t dis=req->disabled;
 __block NSString *result=nil;dispatch_semaphore_t sem=dispatch_semaphore_create(0);
 dispatch_async(dispatch_get_main_queue(), ^{
  NSView *cv=window.contentView;CGFloat w=fmin(720,cv.bounds.size.width-40),h=104;
  TOTKTextBar *bar=[[TOTKTextBar alloc] initWithFrame:NSMakeRect(floor((cv.bounds.size.width-w)/2),48,w,h)];
  bar.autoresizingMask=NSViewMinXMargin|NSViewMaxXMargin|NSViewMaxYMargin;bar.wantsLayer=YES;bar.layer.cornerRadius=12;
  bar.layer.backgroundColor=[NSColor colorWithCalibratedRed:0.035 green:0.075 blue:0.09 alpha:0.92].CGColor;
  bar.maxUnits=maxU;bar.minUnits=minU;bar.digitsOnly=digits;bar.disabled=dis;
  NSTextField *label=[NSTextField labelWithString:prompt];label.font=[NSFont systemFontOfSize:16 weight:NSFontWeightSemibold];
  label.textColor=NSColor.whiteColor;label.frame=NSMakeRect(18,h-36,w-36,22);label.lineBreakMode=NSLineBreakByTruncatingTail;
  NSString *limit=maxU>0?[NSString stringWithFormat:@"Up to %d characters · ",maxU]:@"";
  NSTextField *hint=[NSTextField labelWithString:[NSString stringWithFormat:@"%@Type with the keyboard, Return to confirm, Esc to cancel",limit]];
  hint.font=[NSFont systemFontOfSize:12];hint.textColor=[NSColor colorWithCalibratedWhite:0.72 alpha:1];hint.alignment=NSTextAlignmentRight;
  hint.frame=NSMakeRect(18,h-36,w-36,20);hint.autoresizingMask=NSViewWidthSizable;
  NSTextField *field=secure?[[NSSecureTextField alloc] initWithFrame:NSMakeRect(18,16,w-36,34)]:[[NSTextField alloc] initWithFrame:NSMakeRect(18,16,w-36,34)];
  field.font=[NSFont systemFontOfSize:20];field.stringValue=initial;field.delegate=bar;field.bezelStyle=NSTextFieldRoundedBezel;bar.field=field;
  __weak TOTKTextBar *weakBar=bar;
  bar.done=^(NSString *text){result=[text copy];[weakBar removeFromSuperview];[window makeFirstResponder:window.contentView];dispatch_semaphore_signal(sem);};
  [bar addSubview:label];[bar addSubview:hint];[bar addSubview:field];[cv addSubview:bar];openBar=bar;
  runtime_input_clear();
  if(window.miniaturized)[window deminiaturize:nil];
  if(!assume_focus()){[NSApp activateIgnoringOtherApps:YES];[window makeKeyAndOrderFront:nil];}   /* test windows never take the keyboard */
  [window makeFirstResponder:field];
  fprintf(stderr,"[totk] keyboard: text entry bar shown (%s)\n",prompt.UTF8String);
 });
 dispatch_semaphore_wait(sem,DISPATCH_TIME_FOREVER);
 if(!result)return 0;
 snprintf(out,out_size,"%s",result.UTF8String?result.UTF8String:"");
 return 1;
}
static int viReady;
static unsigned displayIdentity,layerIdentity;
static void vi_initialize(void){if(!metalLayer)runtime_fail("VI requires native macOS window");viReady=1;fprintf(stderr,"[totk] native VI display initialized\n");}
static uint32_t vi_open(void **out){if(!viReady||!out)return 0xf601;*out=&displayIdentity;return 0;}
static uint32_t vi_layer(void **out,void *display){if(!out||display!=&displayIdentity)return 0xf601;*out=&layerIdentity;return 0;}
static uint32_t vi_window(void **out,void *layer){if(!out||layer!=&layerIdentity)return 0xf601;*out=front_metal_layer();return 0;}
static uint32_t vi_vsync(void *ev,void *display){
 if(!ev||display!=&displayIdentity)return 0xf601;hle_bind_system_event(ev,1);
 pthread_mutex_lock(&vsyncLock);if(vsyncCount>=16)runtime_fail("VI display event limit");vsyncEvents[vsyncCount++]=ev;pthread_mutex_unlock(&vsyncLock);return 0;
}
void *runtime_front_override(const char *name){
 static const struct{const char *name;void *fn;} entries[]={
 {"_ZN2nn2vi10InitializeEv",vi_initialize},{"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE",vi_open},
 {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE",vi_layer},{"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",vi_window},
 {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE",vi_vsync}};
 for(unsigned i=0;i<sizeof(entries)/sizeof(*entries);i++)if(!strcmp(name,entries[i].name))return entries[i].fn;return NULL;
}

/* ---- Local change: settings window (Cmd-, or the app menu's Settings…) ----
   Values live in NSUserDefaults suite org.macnativegames.totk.settings. Startup options (console mode, render resolution,
   frame rate, counters, advanced) become environment variables in the launcher (app/launcher.m) at the next start; the
   window says so and offers a restart. Picture mode, frame interpolation and the hints apply at once, through the same
   paths as keys 3, 2 and 1, and the window follows those keys (polled while it is open, saved every second). */
int mfx_requested(void);                    /* gfx/nvn/totk_mfx.m */
void dm_gfx_set_flag(const char *name,int value);
int fps_overlay_hints_enabled(void);        /* gfx/nvn/fps_overlay.c */
int mfxup_enabled(void);                    /* gfx/nvn/totk_mfx_up.m: MetalFX render upscale */
void runtime_gamepad_config(int nintendo,double deadzone);   /* runtime/gamepad.m */
static NSUserDefaults *settings_defaults(void){
 static NSUserDefaults *d;static dispatch_once_t once;
 dispatch_once(&once,^{d=[[NSUserDefaults alloc] initWithSuiteName:@"org.macnativegames.totk.settings"];
  [d registerDefaults:@{@"mode":@"docked",@"renderRes":@"900",@"metalfxUpscale":@NO,@"perfLog":@YES,@"asyncCompile":@YES,@"pauseHidden":@YES,@"padLayout":@"labels",@"padDeadzone":@0.12,@"customWidth":@1600,@"customHeight":@900,@"fpsCap":@"unlimited",@"pictureMode":@0,
   @"interpolation":@0,@"showHints":@YES,@"fpsCounter":@YES,@"shaderCounter":@YES,@"loadCounter":@YES,@"startFullscreen":@NO,
   @"warmThreads":@0,@"aliasGiB":@8}];});
 return d;
}
static NSArray<NSString *> *startup_keys(void){return @[@"mode",@"renderRes",@"customWidth",@"customHeight",@"perfLog",@"asyncCompile",@"pauseHidden",@"fpsCap",@"fpsCounter",@"shaderCounter",@"loadCounter",@"warmThreads",@"aliasGiB"];}
static NSDictionary *startupValues;   /* what this run started with */
static int liveApplied;
/* After the first frame: the saved live options (the game has its 2K-capable output and interpolation set up by then). */
static int settings_off(void){const char *e=getenv("TOTK_SETTINGS");return e&&!strcmp(e,"off");}   /* benchmarks: this run's environment only */
static void settings_apply_live(void){
 if(settings_off()){fprintf(stderr,"[totk] settings: TOTK_SETTINGS=off, saved settings not applied\n");return;}
 NSUserDefaults *d=settings_defaults();
 int mode=(int)[d integerForKey:@"pictureMode"],interp=(int)[d integerForKey:@"interpolation"];
 if(runtime_output_mode_get()>=0&&runtime_output_mode_get()!=mode)runtime_output_mode_set(mode);
 if(interp>=0&&interp<=3&&mfx_requested()!=interp)dm_gfx_set_flag("mfx_interp",interp);
 if(!![d boolForKey:@"showHints"]!=!!fps_overlay_hints_enabled())runtime_shortcut_help_toggle();
 if(!![d boolForKey:@"metalfxUpscale"]!=!!mfxup_enabled())dm_gfx_set_flag("mfx_upscale",[d boolForKey:@"metalfxUpscale"]);
 liveApplied=1;
}
@interface TOTKSettings:NSObject<NSWindowDelegate>
@property(strong) NSPanel *panel;
@property(strong) NSPopUpButton *mode,*renderRes,*fpsCap,*picture,*interp,*threads,*gib;
@property(strong) NSTextField *customW,*customH,*restartNote;
@property(strong) NSPopUpButton *backups,*padLayout;
@property(strong) NSSlider *deadzone;
@property(strong) NSTextField *deadzoneValue;
@property(strong) NSButton *pauseHidden,*asyncCompile,*metalfx,*perfLog,*hints,*fpsCounter,*shaderCounter,*loadCounter,*fullscreen,*restart;
@property(strong) NSTimer *timer;
@end
static TOTKSettings *settings;
static NSTextField *note_label(NSString *text){NSTextField *t=[NSTextField wrappingLabelWithString:text];t.preferredMaxLayoutWidth=360;t.font=[NSFont systemFontOfSize:11];t.textColor=NSColor.secondaryLabelColor;return t;}
static NSPopUpButton *popup(NSArray<NSString *> *items,id target){NSPopUpButton *p=[[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];[p addItemsWithTitles:items];p.target=target;p.action=@selector(changed:);return p;}
static NSButton *check(NSString *title,id target){return [NSButton checkboxWithTitle:title target:target action:@selector(changed:)];}
static NSView *grid(NSArray<NSArray *> *rows){
 NSGridView *g=[NSGridView gridViewWithViews:rows];g.rowSpacing=10;g.columnSpacing=12;
 [g columnAtIndex:0].xPlacement=NSGridCellPlacementTrailing;
 for(NSInteger i=0;i<g.numberOfRows;i++){NSGridRow *r=[g rowAtIndex:i];r.yPlacement=NSGridCellPlacementCenter;
  if([[rows[i] firstObject] isKindOfClass:NSGridCell.class]||rows[i].count==1)[r mergeCellsInRange:NSMakeRange(0,2)];}
 g.translatesAutoresizingMaskIntoConstraints=NO;
 NSView *box=[NSView new];[box addSubview:g];
 [NSLayoutConstraint activateConstraints:@[[g.topAnchor constraintEqualToAnchor:box.topAnchor constant:18],[g.leadingAnchor constraintEqualToAnchor:box.leadingAnchor constant:20],
  [g.trailingAnchor constraintLessThanOrEqualToAnchor:box.trailingAnchor constant:-20],[g.bottomAnchor constraintLessThanOrEqualToAnchor:box.bottomAnchor constant:-12]]];
 return box;
}
static NSTextField *label(NSString *text){return [NSTextField labelWithString:text];}
@implementation TOTKSettings
-(instancetype)init{
 if(!(self=[super init]))return nil;
 self.mode=popup(@[@"Docked (TV)",@"Handheld"],self);
 self.renderRes=popup(@[@"540p",@"720p",@"900p (Switch docked, dynamic)",@"1080p",@"Custom"],self);
 self.metalfx=check(@"MetalFX upscaling of the 3D image",self);self.perfLog=check(@"Write a performance log (perf.csv)",self);
 self.asyncCompile=check(@"Background shader compiling",self);self.pauseHidden=check(@"Pause when the window is hidden",self);
 self.padLayout=popup(@[@"By label (Xbox A = Switch A)",@"Nintendo positions (bottom button = B)"],self);
 self.deadzone=[NSSlider sliderWithValue:0.12 minValue:0.02 maxValue:0.4 target:self action:@selector(changed:)];[self.deadzone.widthAnchor constraintEqualToConstant:220].active=YES;
 self.deadzoneValue=[NSTextField labelWithString:@""];
 self.backups=[[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
 NSButton *restore=[NSButton buttonWithTitle:@"Restore…" target:self action:@selector(restoreBackup:)];
 NSButton *reveal=[NSButton buttonWithTitle:@"Show in Finder" target:self action:@selector(revealBackups:)];
 self.customW=[NSTextField textFieldWithString:@""];self.customH=[NSTextField textFieldWithString:@""];
 for(NSTextField *f in @[self.customW,self.customH]){f.target=self;f.action=@selector(changed:);[f.widthAnchor constraintEqualToConstant:64].active=YES;}
 NSStackView *custom=[NSStackView stackViewWithViews:@[self.customW,label(@"×"),self.customH,label(@"pixels")]];
 self.picture=popup(@[@"1080p",@"1080p, MetalFX upscaled to 2K",@"Native 2K (1440p)"],self);
 self.fpsCap=popup(@[@"Unlimited (current behaviour)",@"60 FPS",@"30 FPS (like the Switch)"],self);
 self.interp=popup(@[@"Off",@"30+30",@"Half the refresh rate",@"Unlimited"],self);
 self.hints=check(@"Show hints and counters (key 1)",self);
 self.fpsCounter=check(@"FPS counter",self);self.shaderCounter=check(@"Shader counter",self);self.loadCounter=check(@"CPU / GPU load",self);
 self.fullscreen=check(@"Start in full screen",self);
 self.threads=popup(@[@"Automatic (performance cores)",@"1 (no parallel warm-up)",@"2",@"4",@"8"],self);
 self.gib=popup(@[@"4 GiB",@"8 GiB (default)",@"12 GiB"],self);
 NSButton *reset=[NSButton buttonWithTitle:@"Reset All Settings" target:self action:@selector(resetAll:)];
 NSTabView *tabs=[NSTabView new];
 NSArray *tabSpecs=@[
  @[@"Graphics",grid(@[@[label(@"Console mode:"),self.mode],@[[NSGridCell emptyContentView],note_label(@"Handheld uses the game's 720p handheld settings.")],
    @[label(@"Render resolution:"),self.renderRes],@[label(@"Custom:"),custom],
    @[[NSGridCell emptyContentView],note_label(@"Size of the 3D image. Mode and resolution: next launch.")],
    @[label(@"Upscaling:"),self.metalfx],@[[NSGridCell emptyContentView],note_label(@"Off: the game's own upscaler. Applies immediately.")],
    @[label(@"Picture:"),self.picture],@[[NSGridCell emptyContentView],note_label(@"Applies immediately (same as key 3).")]])],
  @[@"Performance",grid(@[@[label(@"Frame rate:"),self.fpsCap],@[[NSGridCell emptyContentView],note_label(@"30 FPS halves the GPU work. Applies on next launch.")],
    @[label(@"Frame interpolation:"),self.interp],@[[NSGridCell emptyContentView],note_label(@"MetalFX frame interpolation. Applies immediately (same as key 2).")],
    @[label(@"Shaders:"),self.asyncCompile],@[[NSGridCell emptyContentView],note_label(@"New shaders compile in the background; an object can be missing for a frame or two instead of the game stalling. Next launch.")]])],
  @[@"Display",grid(@[@[label(@"Window:"),self.fullscreen],@[[NSGridCell emptyContentView],note_label(@"Full screen gives the best performance: macOS Game Mode only engages there.")],
    @[[NSGridCell emptyContentView],self.pauseHidden],@[[NSGridCell emptyContentView],note_label(@"Minimized or fully covered: the game stops and uses no power. Next launch.")],@[label(@"Hints:"),self.hints],@[[NSGridCell emptyContentView],note_label(@"Applies immediately.")],
    @[label(@"Counters:"),self.fpsCounter],@[[NSGridCell emptyContentView],self.shaderCounter],@[[NSGridCell emptyContentView],self.loadCounter],
    @[[NSGridCell emptyContentView],note_label(@"Counters apply on next launch.")],
    @[label(@"Keys:"),note_label(@"1  hints and counters   2  frame interpolation   3  picture   F12  screenshot   ⌘,  settings")]])],
  @[@"Controls",grid(@[@[label(@"Button layout:"),self.padLayout],
    @[[NSGridCell emptyContentView],note_label(@"Nintendo positions: the bottom face button is B and the right one is A, as on a Switch controller.")],
    @[label(@"Stick deadzone:"),[NSStackView stackViewWithViews:@[self.deadzone,self.deadzoneValue]]],
    @[[NSGridCell emptyContentView],note_label(@"Raise it if the camera or Link drifts with the stick released. Both apply immediately.")],
    @[label(@"Keyboard:"),note_label(@"WASD move · arrows camera · J A · K B · I X · U Y · Q/E L/R · ⌥/⇧ ZL/ZR · Return + · Tab −")]])],
  @[@"Saves",grid(@[@[note_label(@"A backup of your saves is made at every launch and every clean quit; the newest 10 are kept. Restoring restarts the game and keeps a backup of the current saves.")],
    @[label(@"Backup:"),self.backups],@[[NSGridCell emptyContentView],[NSStackView stackViewWithViews:@[restore,reveal]]]])],
  @[@"Advanced",grid(@[@[note_label(@"⚠︎ Only change these if you know why. They affect startup and memory layout; wrong values can stop the game from starting (Reset All Settings undoes them).")],
    @[label(@"Logging:"),self.perfLog],@[[NSGridCell emptyContentView],note_label(@"One line a second in ~/Library/Logs/MacNativeGames/perf.csv. Applies on next launch.")],
    @[label(@"Warm-up threads:"),self.threads],@[label(@"Memory block:"),self.gib],
    @[[NSGridCell emptyContentView],note_label(@"The game's alias memory region. 8 GiB fits macOS 27; larger values may not. Applies on next launch.")],
    @[[NSGridCell emptyContentView],reset]])]];
 for(NSArray *t in tabSpecs){NSTabViewItem *i=[NSTabViewItem tabViewItemWithViewController:[NSViewController new]];i.label=t[0];i.view=t[1];[tabs addTabViewItem:i];}
 self.restartNote=note_label(@"Some changes apply on next launch.");
 self.restart=[NSButton buttonWithTitle:@"Restart Game" target:self action:@selector(restartGame:)];
 NSStackView *bottom=[NSStackView stackViewWithViews:@[self.restartNote,self.restart]];bottom.spacing=12;
 NSStackView *root=[NSStackView stackViewWithViews:@[tabs,bottom]];root.orientation=NSUserInterfaceLayoutOrientationVertical;root.alignment=NSLayoutAttributeTrailing;
 root.edgeInsets=NSEdgeInsetsMake(14,16,16,16);root.spacing=12;
 [tabs.widthAnchor constraintEqualToConstant:560].active=YES;[tabs.heightAnchor constraintEqualToConstant:400].active=YES;
 self.panel=[[NSPanel alloc] initWithContentRect:NSMakeRect(0,0,592,480) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable backing:NSBackingStoreBuffered defer:YES];
 self.panel.title=@"Tears of the Kingdom Settings";self.panel.contentView=root;self.panel.releasedWhenClosed=NO;self.panel.delegate=self;
 self.panel.hidesOnDeactivate=NO;self.panel.level=NSFloatingWindowLevel;   /* above the game, also in its full-screen Space */
 self.panel.collectionBehavior=NSWindowCollectionBehaviorFullScreenAuxiliary|NSWindowCollectionBehaviorMoveToActiveSpace;
 [self load];
 return self;
}
-(void)load{
 NSUserDefaults *d=settings_defaults();
 NSArray *r=@[@"540",@"720",@"900",@"1080",@"custom"],*f=@[@"unlimited",@"60",@"30"];
 NSUInteger ri=[r indexOfObject:[d stringForKey:@"renderRes"]],fi=[f indexOfObject:[d stringForKey:@"fpsCap"]];
 [self.mode selectItemAtIndex:[[d stringForKey:@"mode"] isEqualToString:@"handheld"]];
 [self.renderRes selectItemAtIndex:ri==NSNotFound?2:(NSInteger)ri];[self.fpsCap selectItemAtIndex:fi==NSNotFound?0:(NSInteger)fi];
 self.perfLog.state=[d boolForKey:@"perfLog"];
 [self.padLayout selectItemAtIndex:[[d stringForKey:@"padLayout"] isEqualToString:@"nintendo"]];self.deadzone.doubleValue=[d doubleForKey:@"padDeadzone"];
 self.deadzoneValue.stringValue=[NSString stringWithFormat:@"%.0f%%",self.deadzone.doubleValue*100];self.asyncCompile.state=[d boolForKey:@"asyncCompile"];self.pauseHidden.state=[d boolForKey:@"pauseHidden"];
 [self.backups removeAllItems];
 for(NSString *b in [[self backupNames] reverseObjectEnumerator]){[self.backups addItemWithTitle:[self backupTitle:b]];self.backups.lastItem.representedObject=b;}
 if(!self.backups.numberOfItems){[self.backups addItemWithTitle:@"No backups yet"];self.backups.enabled=NO;}else self.backups.enabled=YES;
 self.customW.stringValue=[NSString stringWithFormat:@"%ld",(long)[d integerForKey:@"customWidth"]];self.customH.stringValue=[NSString stringWithFormat:@"%ld",(long)[d integerForKey:@"customHeight"]];
 self.fpsCounter.state=[d boolForKey:@"fpsCounter"];self.shaderCounter.state=[d boolForKey:@"shaderCounter"];self.loadCounter.state=[d boolForKey:@"loadCounter"];
 self.fullscreen.state=[d boolForKey:@"startFullscreen"];
 NSArray *th=@[@0,@1,@2,@4,@8],*gb=@[@4,@8,@12];
 NSUInteger ti=[th indexOfObject:@([d integerForKey:@"warmThreads"])],gi=[gb indexOfObject:@([d integerForKey:@"aliasGiB"])];
 [self.threads selectItemAtIndex:ti==NSNotFound?0:(NSInteger)ti];[self.gib selectItemAtIndex:gi==NSNotFound?1:(NSInteger)gi];
 [self pollLive];[self refresh];
}
/* The live options as the game has them now (keys 1-3 change them too): shown, and saved once the saved ones were applied. */
-(void)pollLive{
 NSUserDefaults *d=settings_defaults();
 int mode=runtime_output_mode_get(),interp=mfx_requested(),hints=fps_overlay_hints_enabled();
 self.picture.enabled=mode>=0;[self.picture selectItemAtIndex:mode>=0?mode:[d integerForKey:@"pictureMode"]];
 [self.interp selectItemAtIndex:interp>=0&&interp<=3?interp:0];self.hints.state=hints;self.metalfx.state=mfxup_enabled();
 if(!liveApplied)return;
 if(mode>=0&&[d integerForKey:@"pictureMode"]!=mode)[d setInteger:mode forKey:@"pictureMode"];
 if([d integerForKey:@"interpolation"]!=interp)[d setInteger:interp forKey:@"interpolation"];
 if([d boolForKey:@"showHints"]!=!!hints)[d setBool:!!hints forKey:@"showHints"];
 if([d boolForKey:@"metalfxUpscale"]!=!!mfxup_enabled())[d setBool:!!mfxup_enabled() forKey:@"metalfxUpscale"];
}
-(void)refresh{
 self.customW.enabled=self.customH.enabled=self.renderRes.indexOfSelectedItem==4;
 NSUserDefaults *d=settings_defaults();BOOL pending=NO;
 for(NSString *k in startup_keys())if(![[d objectForKey:k] isEqual:startupValues[k]])pending=YES;
 self.restartNote.hidden=self.restart.hidden=!pending;
}
-(void)changed:(id)sender{
 NSUserDefaults *d=settings_defaults();
 if(sender==self.mode)[d setObject:self.mode.indexOfSelectedItem==1?@"handheld":@"docked" forKey:@"mode"];
 else if(sender==self.renderRes)[d setObject:@[@"540",@"720",@"900",@"1080",@"custom"][(NSUInteger)self.renderRes.indexOfSelectedItem] forKey:@"renderRes"];
 else if(sender==self.metalfx){BOOL on=self.metalfx.state==NSControlStateValueOn;[d setBool:on forKey:@"metalfxUpscale"];dm_gfx_set_flag("mfx_upscale",on);}
 else if(sender==self.perfLog)[d setBool:self.perfLog.state==NSControlStateValueOn forKey:@"perfLog"];
 else if(sender==self.asyncCompile)[d setBool:self.asyncCompile.state==NSControlStateValueOn forKey:@"asyncCompile"];
 else if(sender==self.padLayout||sender==self.deadzone){
  [d setObject:self.padLayout.indexOfSelectedItem==1?@"nintendo":@"labels" forKey:@"padLayout"];[d setDouble:round(self.deadzone.doubleValue*100)/100 forKey:@"padDeadzone"];
  self.deadzoneValue.stringValue=[NSString stringWithFormat:@"%.0f%%",self.deadzone.doubleValue*100];
  runtime_gamepad_config(self.padLayout.indexOfSelectedItem==1,[d doubleForKey:@"padDeadzone"]);}
 else if(sender==self.pauseHidden)[d setBool:self.pauseHidden.state==NSControlStateValueOn forKey:@"pauseHidden"];
 else if(sender==self.customW||sender==self.customH){
  NSInteger w=MAX(64,MIN(4096,self.customW.integerValue)),h=MAX(64,MIN(4096,self.customH.integerValue));
  self.customW.stringValue=[NSString stringWithFormat:@"%ld",(long)w];self.customH.stringValue=[NSString stringWithFormat:@"%ld",(long)h];[d setInteger:w forKey:@"customWidth"];[d setInteger:h forKey:@"customHeight"];}
 else if(sender==self.fpsCap)[d setObject:@[@"unlimited",@"60",@"30"][(NSUInteger)self.fpsCap.indexOfSelectedItem] forKey:@"fpsCap"];
 else if(sender==self.picture){int m=(int)self.picture.indexOfSelectedItem;[d setInteger:m forKey:@"pictureMode"];runtime_output_mode_set(m);}
 else if(sender==self.interp){int m=(int)self.interp.indexOfSelectedItem;[d setInteger:m forKey:@"interpolation"];dm_gfx_set_flag("mfx_interp",m);}
 else if(sender==self.hints){[d setBool:self.hints.state==NSControlStateValueOn forKey:@"showHints"];if(!!fps_overlay_hints_enabled()!=(self.hints.state==NSControlStateValueOn))runtime_shortcut_help_toggle();}
 else if(sender==self.fpsCounter)[d setBool:self.fpsCounter.state==NSControlStateValueOn forKey:@"fpsCounter"];
 else if(sender==self.shaderCounter)[d setBool:self.shaderCounter.state==NSControlStateValueOn forKey:@"shaderCounter"];
 else if(sender==self.loadCounter)[d setBool:self.loadCounter.state==NSControlStateValueOn forKey:@"loadCounter"];
 else if(sender==self.fullscreen)[d setBool:self.fullscreen.state==NSControlStateValueOn forKey:@"startFullscreen"];
 else if(sender==self.threads)[d setInteger:[@[@0,@1,@2,@4,@8][(NSUInteger)self.threads.indexOfSelectedItem] integerValue] forKey:@"warmThreads"];
 else if(sender==self.gib)[d setInteger:[@[@4,@8,@12][(NSUInteger)self.gib.indexOfSelectedItem] integerValue] forKey:@"aliasGiB"];
 fprintf(stderr,"[totk] settings: changed\n");
 [self refresh];
}
-(void)resetAll:(id)sender{(void)sender;
 NSAlert *a=[NSAlert new];a.messageText=@"Reset all settings?";a.informativeText=@"Every option goes back to its default. Startup options apply on next launch.";
 [a addButtonWithTitle:@"Reset"];[a addButtonWithTitle:@"Cancel"];
 if([a runModal]!=NSAlertFirstButtonReturn)return;
 NSUserDefaults *d=settings_defaults();for(NSString *k in d.dictionaryRepresentation.allKeys)[d removeObjectForKey:k];
 runtime_output_mode_set(0);dm_gfx_set_flag("mfx_interp",0);dm_gfx_set_flag("mfx_upscale",0);if(!fps_overlay_hints_enabled())runtime_shortcut_help_toggle();
 [self load];
}
/* Quit like the window's close button (a save in progress is finished first), then open the app again. */
-(void)restartGame:(id)sender{(void)sender;
 NSString *app=NSProcessInfo.processInfo.arguments[0];
 for(int i=0;i<5;i++)app=app.stringByDeletingLastPathComponent;   /* <app>/Contents/Resources/totk/build/totk-native */
 if(![app.pathExtension isEqualToString:@"app"]){NSBeep();fprintf(stderr,"[totk] settings: restart: not inside an app bundle (%s)\n",app.UTF8String);return;}
 /* The launcher itself, with a clean environment (the game's own variables would otherwise win over the new settings). */
 NSString *launcher=[app stringByAppendingPathComponent:@"Contents/MacOS/TOTK"];
 NSTask *t=[NSTask new];t.executableURL=[NSURL fileURLWithPath:@"/bin/sh"];t.currentDirectoryURL=[NSURL fileURLWithPath:@"/"];
 t.environment=@{@"HOME":NSHomeDirectory(),@"USER":NSUserName(),@"LOGNAME":NSUserName(),@"PATH":@"/usr/bin:/bin:/usr/sbin:/sbin",@"TMPDIR":NSTemporaryDirectory()};
 t.arguments=@[@"-c",@"while kill -0 \"$1\" 2>/dev/null; do sleep 0.2; done; sleep 0.5; exec \"$2\"",@"sh",[NSString stringWithFormat:@"%d",getpid()],launcher];
 t.standardOutput=t.standardError=[NSFileHandle fileHandleWithNullDevice];
 NSError *e=nil;if(![t launchAndReturnError:&e]){fprintf(stderr,"[totk] settings: restart helper failed: %s\n",e.localizedDescription.UTF8String);return;}
 fprintf(stderr,"[totk] settings: restarting the game\n");
 [self.panel orderOut:nil];[window performClose:nil];
}
-(NSString *)backupDir{NSString *p=profile_dir();return p?[[p stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"backups"]:nil;}
-(NSArray<NSString *> *)backupNames{NSMutableArray *r=[NSMutableArray array];
 for(NSString *n in [[NSFileManager.defaultManager contentsOfDirectoryAtPath:[self backupDir] error:nil] sortedArrayUsingSelector:@selector(compare:)])if(n.length>15&&[n characterAtIndex:8]=='-')[r addObject:n];return r;}
-(NSString *)backupTitle:(NSString *)n{   /* 20261018-184012-launch -> "18 Oct 18:40:12 · launch" */
 NSDateFormatter *in=[NSDateFormatter new];in.dateFormat=@"yyyyMMdd-HHmmss";in.locale=[NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
 NSDate *t=[in dateFromString:[n substringToIndex:15]];NSDateFormatter *out=[NSDateFormatter new];out.dateFormat=@"d MMM HH:mm:ss";
 NSString *why=n.length>16?[[n substringFromIndex:16] stringByReplacingOccurrencesOfString:@"-" withString:@" "]:@"";
 return t?[NSString stringWithFormat:@"%@ · %@",[out stringFromDate:t],why]:n;}
-(void)revealBackups:(id)sender{(void)sender;NSString *b=[self backupDir];if(b)[NSWorkspace.sharedWorkspace selectFile:nil inFileViewerRootedAtPath:b];}
-(void)restoreBackup:(id)sender{(void)sender;
 NSString *pick=self.backups.selectedItem.representedObject;if(!pick||!player_game())return;
 NSAlert *a=[NSAlert new];a.messageText=[NSString stringWithFormat:@"Restore the backup from %@?",[self backupTitle:pick]];
 a.informativeText=@"The game restarts, your current saves are backed up first, and the selected backup replaces them.";
 [a addButtonWithTitle:@"Restore and Restart"];[a addButtonWithTitle:@"Cancel"];
 if([a runModal]!=NSAlertFirstButtonReturn)return;
 if(![[pick stringByAppendingString:@"\n"] writeToFile:[[self backupDir] stringByAppendingPathComponent:@"restore-request"] atomically:YES encoding:NSUTF8StringEncoding error:nil]){NSBeep();return;}
 fprintf(stderr,"[totk] settings: restore of %s requested\n",pick.UTF8String);[self restartGame:nil];}
-(void)show:(id)sender{(void)sender;
 [self load];if(!self.panel.visible)[self.panel center];[self.panel makeKeyAndOrderFront:nil];
 if(!self.timer)self.timer=[NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES block:^(NSTimer *t){(void)t;[self pollLive];[self refresh];}];
}
-(void)windowWillClose:(NSNotification *)n{(void)n;[self.timer invalidate];self.timer=nil;[window makeKeyAndOrderFront:nil];}
-(void)quit:(id)sender{(void)sender;[window performClose:nil];}
-(void)screenshot:(id)sender{(void)sender;front_take_screenshot();}
@end
/* Quit from the Dock or an Apple Event: close the game window like its close button (a save in progress is finished). */
@interface TOTKAppDelegate:NSObject<NSApplicationDelegate>
@end
@implementation TOTKAppDelegate
-(NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)a{(void)a;fprintf(stderr,"[totk] quit requested\n");[window performClose:nil];return NSTerminateCancel;}
@end
static TOTKAppDelegate *appDelegate;
/* F12 / Cmd-Shift-S: the next game picture (the canvas, no overlays) as a PNG, written by the present path off the render
   thread (gfx/nvn: dm_gfx_shot_at). Folder: TOTK_SCREENSHOT_DIR, else ~/Documents/TearsOfKingdom/screens. */
void dm_gfx_shot_at(long frames_ahead,const char *path);
static void front_take_screenshot(void){
 const char *e=getenv("TOTK_SCREENSHOT_DIR");
 NSString *dir=e&&*e?@(e):[NSHomeDirectory() stringByAppendingPathComponent:@"Documents/TearsOfKingdom/screens"];
 [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
 NSDateFormatter *f=[NSDateFormatter new];f.dateFormat=@"yyyyMMdd-HHmmss";
 NSString *path=[dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%@-screenshot.png",[f stringFromDate:NSDate.date]]];
 dm_gfx_shot_at(2,path.fileSystemRepresentation);fprintf(stderr,"[totk] screenshot requested: %s\n",path.UTF8String);
}
/* Diagnostic input file (runtime/input.c): "settings show", "settings hide", "settings shot <png path>". */
void runtime_front_settings_cmd(const char *op,const char *arg){
 NSString *o=@(op?op:""),*a=arg?@(arg):nil;
 dispatch_async(dispatch_get_main_queue(),^{
  if([o isEqualToString:@"show"])[settings show:nil];
  else if([o isEqualToString:@"hide"])[settings.panel performClose:nil];
  else if([o isEqualToString:@"minimize"])[window miniaturize:nil];   /* tests of the hidden-window behaviour */
  else if([o isEqualToString:@"restore"])[window deminiaturize:nil];
  else if([o isEqualToString:@"tab"]&&a.length){NSTabView *tv=nil;for(NSView *v in settings.panel.contentView.subviews)if([v isKindOfClass:NSTabView.class])tv=(NSTabView *)v;[tv selectTabViewItemAtIndex:MIN(MAX(a.integerValue,0),5)];}
  else if([o isEqualToString:@"shot"]&&a.length){NSView *v=settings.panel.contentView;
   NSBitmapImageRep *rep=[v bitmapImageRepForCachingDisplayInRect:v.bounds];[v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
   NSImage *img=[[NSImage alloc] initWithSize:v.bounds.size];[img lockFocus];   /* the window background under it (dark mode draws light text on a transparent view) */
   [v.effectiveAppearance performAsCurrentDrawingAppearance:^{[NSColor.windowBackgroundColor setFill];NSRectFill(v.bounds);}];[rep drawInRect:v.bounds];
   NSBitmapImageRep *flat=[[NSBitmapImageRep alloc] initWithFocusedViewRect:v.bounds];[img unlockFocus];
   NSData *png=[flat representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
   fprintf(stderr,"[totk] settings: shot %s %s\n",a.UTF8String,[png writeToFile:a atomically:YES]?"written":"failed");}
  else fprintf(stderr,"[totk] settings: unknown command %s\n",o.UTF8String);
 });
}
static void settings_install(void){
 NSUserDefaults *d=settings_defaults();
 NSMutableDictionary *v=[NSMutableDictionary dictionary];for(NSString *k in startup_keys())v[k]=[d objectForKey:k];startupValues=v;
 settings=[TOTKSettings new];appDelegate=[TOTKAppDelegate new];NSApp.delegate=appDelegate;
 if(!settings_off())runtime_gamepad_config([[d stringForKey:@"padLayout"] isEqualToString:@"nintendo"],[d doubleForKey:@"padDeadzone"]);
 NSMenu *bar=[NSMenu new],*appMenu=[[NSMenu alloc] initWithTitle:@"Tears of the Kingdom"],*winMenu=[[NSMenu alloc] initWithTitle:@"Window"];
 NSMenuItem *appItem=[NSMenuItem new],*winItem=[NSMenuItem new];appItem.submenu=appMenu;winItem.submenu=winMenu;[bar addItem:appItem];[bar addItem:winItem];
 NSMenuItem *it=[appMenu addItemWithTitle:@"Settings…" action:@selector(show:) keyEquivalent:@","];it.target=settings;
 it=[appMenu addItemWithTitle:@"Take Screenshot" action:@selector(screenshot:) keyEquivalent:@"S"];it.target=settings;   /* Cmd-Shift-S */
 [appMenu addItem:NSMenuItem.separatorItem];
 [appMenu addItemWithTitle:@"Hide Tears of the Kingdom" action:@selector(hide:) keyEquivalent:@"h"];
 it=[appMenu addItemWithTitle:@"Quit Tears of the Kingdom" action:@selector(quit:) keyEquivalent:@"q"];it.target=settings;
 [winMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
 it=[winMenu addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];it.keyEquivalentModifierMask=NSEventModifierFlagCommand|NSEventModifierFlagControl;
 NSApp.mainMenu=bar;NSApp.windowsMenu=winMenu;
 /* Saved changes made with keys 1-3 while the window is closed. */
 [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *t){(void)t;if(!settings.panel.visible&&liveApplied)[settings pollLive];}];
 if([d boolForKey:@"startFullscreen"])dispatch_after(dispatch_time(DISPATCH_TIME_NOW,500*NSEC_PER_MSEC),dispatch_get_main_queue(),^{if(!(window.styleMask&NSWindowStyleMaskFullScreen))[window toggleFullScreen:nil];});
 fprintf(stderr,"[totk] settings: %s\n",[[d dictionaryRepresentation] objectsForKeys:@[@"mode",@"renderRes",@"metalfxUpscale",@"fpsCap",@"pictureMode",@"interpolation"] notFoundMarker:@"-"].description.UTF8String);
}
