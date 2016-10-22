#include <cstdio>
#include <cstdarg>
#include "types.h"

#include <sys/stat.h>
#include <retro_stat.h>

#include "../hw/pvr/pvr_regs.h"

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
#include <glsm/glsm.h>
#endif
#include "../rend/rend.h"

#include "libretro.h"

int screen_width  = 640;
int screen_height = 480;
bool boot_to_bios;

u16 kcode[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
u8 rt[4] = {0, 0, 0, 0};
u8 lt[4] = {0, 0, 0, 0};
u32 vks[4];
s8 joyx[4], joyy[4];

bool enable_purupuru = true;

bool inside_loop     = true;
static bool first_run = true;

enum DreamcastController
{
	DC_BTN_C       = 1,
	DC_BTN_B       = 1<<1,
	DC_BTN_A       = 1<<2,
	DC_BTN_START   = 1<<3,
	DC_DPAD_UP     = 1<<4,
	DC_DPAD_DOWN   = 1<<5,
	DC_DPAD_LEFT   = 1<<6,
	DC_DPAD_RIGHT  = 1<<7,
	DC_BTN_Z       = 1<<8,
	DC_BTN_Y       = 1<<9,
	DC_BTN_X       = 1<<10,
	DC_BTN_D       = 1<<11,
	DC_DPAD2_UP    = 1<<12,
	DC_DPAD2_DOWN  = 1<<13,
	DC_DPAD2_LEFT  = 1<<14,
	DC_DPAD2_RIGHT = 1<<15,

	DC_AXIS_LT = 0X10000,
	DC_AXIS_RT = 0X10001,
	DC_AXIS_X  = 0X20000,
	DC_AXIS_Y  = 0X20001,
};

struct retro_perf_callback perf_cb;
retro_get_cpu_features_t perf_get_cpu_features_cb = NULL;

// Callbacks
retro_log_printf_t         log_cb = NULL;
retro_video_refresh_t      video_cb = NULL;
retro_input_poll_t         poll_cb = NULL;
retro_input_state_t        input_cb = NULL;
retro_audio_sample_batch_t audio_batch_cb = NULL;
retro_environment_t        environ_cb = NULL;
static retro_rumble_interface rumble;

int dc_init(int argc,wchar* argv[]);
void dc_run();

static int co_argc;
static wchar** co_argv;

char *game_data;
char game_dir[1024];
char game_dir_no_slash[1024];

void co_dc_yield(void)
{
   if (settings.UpdateMode || settings.UpdateModeForced)
      return;
   inside_loop = false;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   // Nothing to do here
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_cb = cb;
}

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   struct retro_variable variables[] = {
#if ((FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X86) || (HOST_CPU == CPU_ARM) || (HOST_CPU == CPU_X64) || TARGET_NO_JIT)
      {
         "reicast_cpu_mode",
         "CPU Mode (restart); "
#if (FEAT_SHREC == DYNAREC_JIT && HOST_CPU == CPU_X86) || (HOST_CPU == CPU_ARM) || (HOST_CPU == CPU_X64)
            "dynamic_recompiler"
#endif
#ifdef TARGET_NO_JIT
            "|"
            "generic_recompiler"
#endif
            ,
      },
#endif
      {
         "reicast_boot_to_bios",
         "Boot to BIOS (restart); disabled|enabled",
      },
      {
         "reicast_internal_resolution",
         "Internal resolution; 640x480|1280x960|1920x1440|2560x1920|3200x2400|3840x2880|4480x3360",
      },
      {
         "reicast_mipmapping",
         "Mipmapping; enabled|disabled",
      },
      {
         "reicast_volume_modifier_mode",
         "Volume modifier mode; disabled|debug|on|full",
      },
      {
         "reicast_widescreen_hack",
         "Widescreen hack; disabled|enabled",
      },
      {
         "reicast_audio_buffer_size",
         "Audio buffer size; 1024|2048",
      },
      {
         "reicast_cable_type",
         "Cable type; TV (VBS/Y+S/C)|TV (RGB)|VGA (RGB)",
      },
      {
         "reicast_broadcast",
         "Broadcast; 4|0|1|2|3",
      },
      {
         "reicast_framerate",
         "Framerate; normal|fullspeed",
      },
      {
         "reicast_region",
         "Region; 3|0|1|2",
      },
      {
         "reicast_precompile_shaders",
         "Precompile shaders; disabled|enabled",
      },
      {
         "reicast_enable_rtt",
         "Enable RTT (Render To Texture); enabled|disabled", 
      },
      {
         "reicast_enable_purupuru",
         "Purupuru Pack (restart); enabled|disabled"
      },
      { NULL, NULL },
   };

   cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
}


// Now comes the interesting stuff
void retro_init(void)
{
   // Logging
   struct retro_log_callback log;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb))
      perf_get_cpu_features_cb = perf_cb.get_cpu_features;
   else
      perf_get_cpu_features_cb = NULL;

   // Set color mode
   unsigned color_mode = RETRO_PIXEL_FORMAT_XRGB8888;
   environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &color_mode);
}

void dc_term(void);


void retro_deinit(void)
{
   dc_term();
   first_run = true;
}

bool enable_rtt                 = true;
static bool is_dupe             = false;
static bool initialize_renderer = false;
static bool resize_renderer     = false;
static unsigned old_width       = 0;
static unsigned old_height      = 0;
void rend_initialization();

static void update_variables(bool first_boot)
{
   static bool widescreen_set = false;
   struct retro_variable var = {
      .key = "reicast_internal_resolution",
   };

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      char *pch;
      char str[100];
      snprintf(str, sizeof(str), "%s", var.value);
      
      pch = strtok(str, "x");
      if (pch)
         screen_width = strtoul(pch, NULL, 0);
      pch = strtok(NULL, "x");
      if (pch)
         screen_height = strtoul(pch, NULL, 0);

      if (first_boot)
      {
         old_width       = screen_width;
         old_height      = screen_height;
      }
      else
      {
         if (screen_height != old_height || screen_width != old_width)
         {
            old_width       = screen_width;
            old_height      = screen_height;
            resize_renderer = true;
         }
      }

      fprintf(stderr, "[reicast]: Got size: %u x %u.\n", screen_width, screen_height);
   }
   else
   {
      old_width       = screen_width;
      old_height      = screen_height;
   }

   var.key = "reicast_cpu_mode";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "dynamic_recompiler"))
         settings.dynarec.Type = 0;
      else if (!strcmp(var.value, "generic_recompiler"))
         settings.dynarec.Type = 1;
   }

   var.key = "reicast_boot_to_bios";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         boot_to_bios = true;
      else if (!strcmp(var.value, "disabled"))
         boot_to_bios = false;
   }
   else
      boot_to_bios = false;

   var.key = "reicast_mipmapping";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         settings.rend.UseMipmaps		= 1;
      else if (!strcmp(var.value, "disabled"))
         settings.rend.UseMipmaps		= 0;
   }
   else
      settings.rend.UseMipmaps		= 1;

   var.key = "reicast_volume_modifier_mode";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         settings.pvr.Emulation.ModVolMode		= 0;
      else if (!strcmp(var.value, "debug"))
         settings.pvr.Emulation.ModVolMode		= 1;
      else if (!strcmp(var.value, "on"))
         settings.pvr.Emulation.ModVolMode		= 2;
      else if (!strcmp(var.value, "full"))
         settings.pvr.Emulation.ModVolMode		= 3;
   }
   else
      settings.rend.UseMipmaps		= 1;

   var.key = "reicast_widescreen_hack";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         settings.rend.WideScreen = true;
      else
         settings.rend.WideScreen = false;
   }

   var.key = "reicast_audio_buffer_size";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      settings.aica.BufferSize = atoi(var.value);
   }
   else
      settings.aica.BufferSize = 1024;

   var.key = "reicast_cable_type";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("VGA (RGB)", var.value))
         settings.dreamcast.cable = 0;
      else if (!strcmp("TV (RGB)", var.value))
         settings.dreamcast.cable = 2;
      else if (!strcmp("TV (VBS/Y+S/C)", var.value))
         settings.dreamcast.cable = 3;
   }

   var.key = "reicast_broadcast";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("0", var.value))
         settings.dreamcast.broadcast = 0;
      else if (!strcmp("1", var.value))
         settings.dreamcast.broadcast = 1;
      else if (!strcmp("2", var.value))
         settings.dreamcast.broadcast = 2;
      else if (!strcmp("3", var.value))
         settings.dreamcast.broadcast = 3;
      else if (!strcmp("4", var.value))
         settings.dreamcast.broadcast = 4;
   }
   else
         settings.dreamcast.broadcast = 4;

   var.key = "reicast_framerate";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("normal", var.value))
         settings.UpdateMode = 0;
      else if (!strcmp("fullspeed", var.value))
         settings.UpdateMode = 1;
   }
   else
      settings.UpdateMode = 0;

   var.key = "reicast_region";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("0", var.value))
         settings.dreamcast.region = 0;
      else if (!strcmp("1", var.value))
         settings.dreamcast.region = 1;
      else if (!strcmp("2", var.value))
         settings.dreamcast.region = 2;
      else if (!strcmp("3", var.value))
         settings.dreamcast.region = 3;
   }
   else
         settings.dreamcast.region = 3;

   var.key = "reicast_precompile_shaders";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("disabled", var.value))
         settings.pvr.Emulation.precompile_shaders = false;
      else
         settings.pvr.Emulation.precompile_shaders = true;
   }

   var.key = "reicast_enable_rtt";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp("disabled", var.value))
         enable_rtt = false;
      else
         enable_rtt = true;
   }

   var.key = "reicast_enable_purupuru";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      enable_purupuru = (strcmp("enabled", var.value) == 0);
}


void retro_run (void)
{
   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables(false);

   if (first_run)
   {
      dc_init(co_argc,co_argv);
      dc_run();
      first_run = false;
      return;
   }

   if (initialize_renderer)
   {
      rend_initialization();
      initialize_renderer = false;
   }
   else if (resize_renderer)
   {
      struct retro_system_av_info new_av_info;
      retro_get_system_av_info(&new_av_info);

      rend_resize(screen_width, screen_height);
      resize_renderer = false;

      environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &new_av_info);
      environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO,
               &new_av_info);
   }

   dc_run();
#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
   video_cb(is_dupe ? 0 : RETRO_HW_FRAME_BUFFER_VALID, screen_width, screen_height, 0);
#endif
   is_dupe     = true;
   inside_loop = true;
}

void retro_reset (void)
{
   //TODO
   dc_term();
   first_run = true;
   settings.dreamcast.cable = 3;
   update_variables(false);
}


#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)

static void context_reset(void)
{
   printf("context_reset.\n");
   glsm_ctl(GLSM_CTL_STATE_CONTEXT_RESET, NULL);
   initialize_renderer = true;

}

static void context_destroy(void)
{
   glsm_ctl(GLSM_CTL_STATE_CONTEXT_DESTROY, NULL);
   rend_term();
}
#endif

static void extract_directory(char *buf, const char *path, size_t size)
{
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   char *base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
      buf[0] = '\0';
}

// Loading/unloading games
bool retro_load_game(const struct retro_game_info *game)
{
   glsm_ctx_params_t params = {0};
   const char *dir = NULL;
#ifdef _WIN32
   char slash = '\\';
#else
   char slash = '/';
#endif

   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L (fierce)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L (weak)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R (fierce)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R (weak)" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },

      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L (fierce)" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L (weak)" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R (fierce)" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R (weak)" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },

      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L (fierce)" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L (weak)" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R (fierce)" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R (weak)" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },

      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L (fierce)" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L (weak)" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R (fierce)" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R (weak)" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Analog X" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Analog Y" },

      { 0 },
   };


   extract_directory(game_dir, game->path, sizeof(game_dir));

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   if (environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble) && log_cb)
        log_cb(RETRO_LOG_INFO, "Rumble interface supported!\n");

   if (!(environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir))
      dir = game_dir;

   snprintf(game_dir, sizeof(game_dir), "%s%cdc%c", dir, slash, slash);
   snprintf(game_dir_no_slash, sizeof(game_dir_no_slash), "%s%cdc", dir, slash);

   settings.dreamcast.cable = 3;
   update_variables(true);

   if (game->path[0] == '\0')
      boot_to_bios = true;

   if (!boot_to_bios)
      game_data = strdup(game->path);

   {
      char data_dir[1024];

      snprintf(data_dir, sizeof(data_dir), "%s%s", game_dir, "data");

      printf("Creating dir: %s\n", data_dir);
      struct stat buf;
      if (stat(data_dir, &buf) < 0)
      {
         mkdir_norecurse(data_dir);
      }
   }

#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
   params.context_reset         = context_reset;
   params.context_destroy       = context_destroy;
   params.environ_cb            = environ_cb;
   params.stencil               = true;
   params.imm_vbo_draw          = NULL;
   params.imm_vbo_disable       = NULL;

   if (!glsm_ctl(GLSM_CTL_STATE_CONTEXT_INIT, &params))
      return false;
#endif


   return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
   return false; //TODO (?)
}

void retro_unload_game(void)
{
   if (game_data)
      free(game_data);
   game_data = NULL;
}


// Memory/Serialization
void *retro_get_memory_data(unsigned type)
{
   return 0; //TODO
}

size_t retro_get_memory_size(unsigned type)
{
   return 0; //TODO
}

size_t retro_serialize_size (void)
{
   return 0; //TODO
}

bool retro_serialize(void *data, size_t size)
{
   return false; //TODO
}

bool retro_unserialize(const void * data, size_t size)
{
   return false; //TODO
}

// Cheats
void retro_cheat_reset(void)
{
   // Nothing to do here
}
void retro_cheat_set(unsigned unused, bool unused1, const char* unused2)
{
   // Nothing to do here
}


// Get info
const char* retro_get_system_directory(void)
{
   const char* dir;
   environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir);
   return dir ? dir : ".";
}

void retro_get_system_info(struct retro_system_info *info)
{
   info->library_name = "Reicast";
   info->library_version = "0.1";
   info->valid_extensions = "cdi|gdi|chd|cue";
   info->need_fullpath = true;
   info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   /*                        00=VGA    01=NTSC   10=PAL,   11=illegal/undocumented */
   const int spg_clks[4] = { 26944080, 13458568, 13462800, 26944080 };
	u32 pixel_clock= spg_clks[(SPG_CONTROL.full >> 6) & 3];

   info->geometry.base_width   = screen_width;
   info->geometry.base_height  = screen_height;
   info->geometry.max_width    = screen_width;
   info->geometry.max_height   = screen_height;
   info->geometry.aspect_ratio = 4.0 / 3.0;

   switch (pixel_clock)
   {
      case 26944080:
         info->timing.fps = 60.00; /* (VGA  480 @ 60.00) */
         break;
      case 26917135:
         info->timing.fps = 59.94; /* (NTSC 480 @ 59.94) */
         break;
      case 13462800:
         info->timing.fps = 50.00; /* (PAL 240  @ 50.00) */
         break;
      case 13458568:
         info->timing.fps = 59.94; /* (NTSC 240 @ 59.94) */
         break;
      case 25925600:
         info->timing.fps = 50.00; /* (PAL 480  @ 50.00) */
         break;
   }

   info->timing.sample_rate = 44100.0;
}

unsigned retro_get_region (void)
{
   return RETRO_REGION_NTSC; //TODO
}


// Controller
void retro_set_controller_port_device(unsigned in_port, unsigned device)
{
   //TODO
   if (rumble.set_rumble_state)
   {
      rumble.set_rumble_state(in_port, RETRO_RUMBLE_STRONG, 0);
      rumble.set_rumble_state(in_port, RETRO_RUMBLE_WEAK,   0);
   }
}


// API version (to detect version mismatch)
unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

//Reicast stuff
void os_DoEvents(void)
{
   is_dupe = false;
   poll_cb();

   if (settings.UpdateMode || settings.UpdateModeForced)
   {
      inside_loop = false;
      rend_end_render();
   }
}

void os_CreateWindow()
{
   // Nothing to do here
}

bool update_zmax;
bool update_zmin;

void UpdateInputState(u32 port)
{
   int id;
   static const uint16_t joymap[] = {
      /* JOYPAD_B      */ DC_BTN_A,
      /* JOYPAD_Y      */ DC_BTN_X,
      /* JOYPAD_SELECT */ 0,
      /* JOYPAD_START  */ DC_BTN_START,
      /* JOYPAD_UP     */ DC_DPAD_UP,
      /* JOYPAD_DOWN   */ DC_DPAD_DOWN,
      /* JOYPAD_LEFT   */ DC_DPAD_LEFT,
      /* JOYPAD_RIGHT  */ DC_DPAD_RIGHT,
      /* JOYPAD_A      */ DC_BTN_B,
      /* JOYPAD_X      */ DC_BTN_Y,
      /* JOYPAD_L      */ 0, /* full L */
      /* JOYPAD_R      */ 0, /* full R */
      /* JOYPAD_L2     */ 0, /* half L */
      /* JOYPAD_R2     */ 0, /* half R */
      /* JOYPAD_L3     */ 0,
      /* JOYPAD_R3     */ 0,
   };


   rt[port] = 0;
   lt[port] = 0;

   for (id = RETRO_DEVICE_ID_JOYPAD_B; id < RETRO_DEVICE_ID_JOYPAD_R3+1; ++id)
   {
      uint16_t dc_key = joymap[id];
      bool is_down = input_cb(port, RETRO_DEVICE_JOYPAD, 0, id);

      switch (id)
      {
         case RETRO_DEVICE_ID_JOYPAD_L2:
            lt[port] |= 0x7f * is_down;
            break;
         case RETRO_DEVICE_ID_JOYPAD_R2:
            rt[port] |= 0x7f * is_down;
            break;
         case RETRO_DEVICE_ID_JOYPAD_L:
            lt[port] |= 0xff * is_down;
            break;
         case RETRO_DEVICE_ID_JOYPAD_R:
            rt[port] |= 0xff * is_down;
            break;
         default:
            if (is_down)
               kcode[port] &= ~dc_key;
            else
               kcode[port] |= dc_key;

      }
   }

#if 0
   {
      static f32 InitialzMax = -999.0f;
      static f32 InitialzMin = -999.0f;
      int16_t rstick_x = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X) / 256;
      int16_t rstick_y = input_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y) / 256;

      if (InitialzMax == -999.0f)
         InitialzMax = settings.pvr.Emulation.zMax;
      if (InitialzMin == -999.0f)
         InitialzMin = settings.pvr.Emulation.zMin;
     
      if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3))
      {
         if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2))
            settings.pvr.Emulation.zMax = 1.0;
         else
            settings.pvr.Emulation.zMax = InitialzMax;
         update_zmax = true;
      }

      if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3))
      {
         if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2))
            settings.pvr.Emulation.zMin = 0.0;
         else
            settings.pvr.Emulation.zMin = InitialzMin;
         update_zmin = true;
      }

      if (rstick_y <= -16 && rstick_y >= -0x8000)
      {
         settings.pvr.Emulation.zMin -= 0.1f;
         update_zmin = true;
      }
      if (rstick_y >= 16 && rstick_y <= 0x7fff)
      {
         settings.pvr.Emulation.zMin += 0.1f;
         update_zmin = true;
      }

      if (rstick_x <= -16 && rstick_x >= -0x8000)
      {
         settings.pvr.Emulation.zMax -= 0.1f;
         update_zmax = true;
      }
      if (rstick_x >= 16 && rstick_x <= 0x7fff)
      {
         settings.pvr.Emulation.zMax += 0.1f;
         update_zmax = true;
      }
   }
#endif

   joyx[port] = input_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X) / 256;
   joyy[port] = input_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y) / 256;
}

void UpdateVibration(u32 port, u32 value)
{
   if (!rumble.set_rumble_state)
      return;

   u8 POW_POS = (value >> 8) & 0x3;
   u8 POW_NEG = (value >> 12) & 0x3;
   u8 FREQ = (value >> 16) & 0xFF;

   double pow = (POW_POS + POW_NEG) / 7.0;
   double pow_l = pow * (0x3B - FREQ) / 17.0;
   double pow_r = pow * (FREQ - 0x07) / 15.0;

   if (pow_l > 1.0) pow_l = 1.0;
   if (pow_r > 1.0) pow_r = 1.0;

   rumble.set_rumble_state(port, RETRO_RUMBLE_STRONG, (u16)(65535 * pow_l));
   rumble.set_rumble_state(port, RETRO_RUMBLE_WEAK,   (u16)(65535 * pow_r));
}

void* libPvr_GetRenderTarget()
{
   return NULL;
}

void* libPvr_GetRenderSurface()
{
   return NULL;
}

int msgboxf(const char* text, unsigned int type, ...)
{
   if (log_cb)
   {
      va_list args;

      char temp[2048];

      va_start(args, type);
      vsprintf(temp, text, args);
      va_end(args);

      log_cb(RETRO_LOG_INFO, temp);
   }
   return 0;
}

int get_mic_data(u8* buffer) { return 0; }
int push_vmu_screen(u8* buffer) { return 0; }

void os_DebugBreak(void)
{
   printf("DEBUGBREAK!\n");
   exit(-1);
}
