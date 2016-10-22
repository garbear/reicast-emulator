#include "Renderer_if.h"
#include "ta.h"
#include "hw/pvr/pvr_mem.h"
#include "TexCache.h"

/*

	rendv3 ideas
	- multiple backends
	  - ESish
	    - OpenGL ES2.0
	    - OpenGL ES3.0
	    - OpenGL 3.1
	  - OpenGL 4.x
	  - Direct3D 10+ ?
	- correct memory ordering model
	- resource pools
	- threaded TA
	- threaded rendering
	- RTTs
	- framebuffers
	- overlays


	PHASES
	- TA submission (memops, dma)

	- TA parsing (defered, rend thread)

	- CORE render (in-order, defered, rend thread)


	submission is done in-order
	- Partial handling of TA values
	- Gotchas with TA contexts

	parsing is done on demand and out-of-order, and might be skipped
	- output is only consumed by renderer

	render is queued on RENDER_START, and won't stall the emulation or might be skipped
	- VRAM integrity is an issue with out-of-order or delayed rendering.
	- selective vram snapshots require TA parsing to complete in order with REND_START / REND_END


	Complications
	- For some apis (gles2, maybe gl31) texture allocation needs to happen on the gpu thread
	- multiple versions of different time snapshots of the same texture are required
	- TA parsing vs frameskip logic


	Texture versioning and staging
	 A memory copy of the texture can be used to temporary store the texture before upload to vram
	 This can be moved to another thread
	 If the api supports async resource creation, we don't need the extra copy
	 Texcache lookups need to be versioned


	rendv2x hacks
	- Only a single pending render. Any renders while still pending are dropped (before parsing)
	- wait and block for parse/texcache. Render is async
*/

u32 VertexCount=0;
u32 FrameCount=1;

Renderer* renderer;

#if !defined(TARGET_NO_THREADS)
cResetEvent rs;
cResetEvent re;
sthread_t *rthd;
#endif

bool pend_rend = false;

int max_idx,max_mvo,max_op,max_pt,max_tr,max_vtx,max_modt, ovrn;

TA_context* _pvrrc;
void SetREP(TA_context* cntx);

#if defined(TARGET_NO_THREADS)
static bool rend_frame(TA_context* ctx, bool draw_osd)
{
   return renderer->Process(ctx) && renderer->Render();
}

void rend_end_render(void)
{
   if (pend_rend)
      renderer->Present();
}

void rend_term(void)
{ 
   renderer->Term();
}
#else
static bool rend_frame(TA_context* ctx, bool draw_osd)
{
   bool proc = renderer->Process(ctx);
   slock_lock(re.mutx);
   re.state = true;
   scond_signal(re.cond);
   slock_unlock(re.mutx);

   return proc && renderer->Render();
}

void rend_end_render(void)
{
   if (pend_rend)
   {
      slock_lock(re.mutx);
      if (!re.state)
         scond_wait( re.cond, re.mutx );
      re.state=false;
      slock_unlock(re.mutx);
   }
}

void rend_term(void)
{
   sthread_join(rthd);

   rthd = NULL;

   slock_free(re.mutx);
   slock_free(rs.mutx);
   scond_free(re.cond);
   scond_free(rs.cond);
   re.mutx = NULL;
   rs.mutx = NULL;
   re.cond = NULL;
   rs.cond = NULL;
}
#endif

static bool rend_single_frame(void)
{
   //wait render start only if no frame pending
   do
   {
#if !defined(TARGET_NO_THREADS)
      slock_lock(rs.mutx);
      if (!rs.state)
         scond_wait( rs.cond, rs.mutx );
      rs.state=false;
      slock_unlock(rs.mutx);
#endif
      _pvrrc = DequeueRender();
   }
   while (!_pvrrc);
   bool do_swp = rend_frame(_pvrrc, true);

   //clear up & free data ..
   FinishRender(_pvrrc);
   _pvrrc=0;

   return do_swp;
}

extern int screen_width;
extern int screen_height;

void rend_initialization(void)
{
#if SET_AFNT
   cpu_set_t mask;

   /* CPU_ZERO initializes all the bits in the mask to zero. */
   CPU_ZERO( &mask );
   /* CPU_SET sets only the bit corresponding to cpu. */
   CPU_SET( 1, &mask );

   /* sched_setaffinity returns 0 in success */

   if( sched_setaffinity( 0, sizeof(mask), &mask ) == -1 )
      printf("WARNING: Could not set CPU Affinity, continuing...\n");
#endif

   if (!renderer->Init())
      die("rend->init() failed\n");
   rend_resize(screen_width, screen_height);
}

static void *rend_thread(void* p)
{
   rend_initialization();

   for(;;)
   {
      if (rend_single_frame())
         renderer->Present();
   }
}


void rend_resize(int width, int height)
{
	renderer->Resize(width, height);
}


void rend_start_render(void)
{
   pend_rend = false;
   TA_context* ctx = tactx_Pop(CORE_CURRENT_CTX);

   SetREP(ctx);

   if (!ctx)
      return;

   if (ctx->rend.Overrun)
   {
      ovrn++;
      printf("WARNING: Rendering context is overrun (%d), aborting frame\n",ovrn);
      tactx_Recycle(ctx);
      return;
   }

   //printf("REP: %.2f ms\n",render_end_pending_cycles/200000.0);
   FillBGP(ctx);

   ctx->rend.isRTT      = (FB_W_SOF1& 0x1000000)!=0;
   ctx->rend.isAutoSort = UsingAutoSort();

   ctx->rend.fb_X_CLIP  = FB_X_CLIP;
   ctx->rend.fb_Y_CLIP  = FB_Y_CLIP;

   max_idx              = max(max_idx,  ctx->rend.idx.used());
   max_vtx              = max(max_vtx,  ctx->rend.verts.used());
   max_op               = max(max_op,   ctx->rend.global_param_op.used());
   max_pt               = max(max_pt,   ctx->rend.global_param_pt.used());
   max_tr               = max(max_tr,   ctx->rend.global_param_tr.used());

   max_mvo              = max(max_mvo,  ctx->rend.global_param_mvo.used());
   max_modt             = max(max_modt, ctx->rend.modtrig.used());

#if 0
   printf("max: idx: %d, vtx: %d, op: %d, pt: %d, tr: %d, mvo: %d, modt: %d, ov: %d\n", max_idx, max_vtx, max_op, max_pt, max_tr, max_mvo, max_modt, ovrn);
#endif
   if (QueueRender(ctx) || !settings.QueueRender)
   {
      palette_update();
#if !defined(TARGET_NO_THREADS)
      slock_lock(rs.mutx);
      rs.state=true;
      scond_signal(rs.cond);
      slock_unlock(rs.mutx);
#else
      rend_single_frame();
#endif
      pend_rend = true;
   }
}

bool rend_init(void)
{
#ifdef NO_REND
	renderer	 = rend_norend();
#else
	renderer = rend_GLES2();
#endif

#if !defined(TARGET_NO_THREADS)
   rthd = (sthread_t*)sthread_create(rend_thread, 0);

   rs.mutx = slock_new();
   rs.cond = scond_new();
   re.mutx = slock_new();
   re.cond = scond_new();
#endif

	return true;
}

void rend_vblank(void)
{
	os_DoEvents();
}
