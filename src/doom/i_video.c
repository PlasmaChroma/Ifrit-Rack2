#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include "doomtype.h"
#include "i_video.h"

char *video_driver = "headless";
boolean screenvisible = true;
boolean screensaver_mode = false;
int usegamma = 0;
pixel_t *I_VideoBuffer = NULL;

int screen_width = SCREENWIDTH;
int screen_height = SCREENHEIGHT;
int fullscreen = 0;
int aspect_ratio_correct = 1;
int integer_scaling = 0;
int vga_porch_flash = 0;
int force_software_renderer = 1;
char *window_position = "center";
unsigned int joywait = 0;

void I_GetWindowPosition(int *x, int *y, int w, int h)
{
    if (x) *x = 0;
    if (y) *y = 0;
}

void I_InitGraphics(void)
{
    I_VideoBuffer = malloc(SCREENWIDTH * SCREENHEIGHT * sizeof(pixel_t));
    memset(I_VideoBuffer, 0, SCREENWIDTH * SCREENHEIGHT * sizeof(pixel_t));
}

void I_GraphicsCheckCommandLine(void)
{
}

void I_ShutdownGraphics(void)
{
    if (I_VideoBuffer)
    {
        free(I_VideoBuffer);
        I_VideoBuffer = NULL;
    }
}

static byte active_palette[768];
static uint8_t *doom_rgba_buffer = NULL;
static pthread_mutex_t doom_rgba_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int doom_dirty_frame = 0;
static _Atomic int doom_exit_requested = 0;
int usemouse = 1;
static _Atomic int g_game_health = 100;
static _Atomic int g_game_frag_trigger = 0;
static _Atomic float g_cv_xmove = 0.0f;
static _Atomic float g_cv_ymove = 0.0f;
static _Atomic int g_cv_fire = 0;
static _Atomic int g_cv_weapon = -1;
static _Atomic int g_cv_xmove_mode = 1;
static _Atomic int g_cv_warp_epsd = 0;
static _Atomic int g_cv_warp_map = 0;
static _Atomic int g_cv_cheat_request = 0;
static _Atomic int g_cv_save_request = 0;

void I_SetRackCvControls(float xmove, float ymove, int fire, int weapon, int xmove_mode)
{
    atomic_store_explicit(&g_cv_xmove, xmove, memory_order_relaxed);
    atomic_store_explicit(&g_cv_ymove, ymove, memory_order_relaxed);
    atomic_store_explicit(&g_cv_fire, fire, memory_order_relaxed);
    atomic_store_explicit(&g_cv_weapon, weapon, memory_order_relaxed);
    atomic_store_explicit(&g_cv_xmove_mode, xmove_mode, memory_order_release);
}

void I_GetRackCvControls(float *xmove, float *ymove, int *fire, int *weapon, int *xmove_mode)
{
    if (xmove)
        *xmove = atomic_load_explicit(&g_cv_xmove, memory_order_acquire);
    if (ymove)
        *ymove = atomic_load_explicit(&g_cv_ymove, memory_order_acquire);
    if (fire)
        *fire = atomic_load_explicit(&g_cv_fire, memory_order_acquire);
    if (weapon)
        *weapon = atomic_load_explicit(&g_cv_weapon, memory_order_acquire);
    if (xmove_mode)
        *xmove_mode = atomic_load_explicit(&g_cv_xmove_mode, memory_order_acquire);
}

void I_GetRackGameState(int *health, int *frag_triggered)
{
    if (health)
        *health = atomic_load_explicit(&g_game_health, memory_order_acquire);
    if (frag_triggered)
        *frag_triggered = atomic_exchange_explicit(&g_game_frag_trigger, 0, memory_order_acq_rel);
}

void I_SetRackGameHealth(int health)
{
    atomic_store_explicit(&g_game_health, health, memory_order_release);
}

void I_SetRackFragTrigger(void)
{
    atomic_store_explicit(&g_game_frag_trigger, 1, memory_order_release);
}

void I_RequestRackWarp(int episode, int map)
{
    atomic_store_explicit(&g_cv_warp_epsd, episode, memory_order_relaxed);
    atomic_store_explicit(&g_cv_warp_map, map, memory_order_release);
}

int I_TakeRackWarp(int *episode, int *map)
{
    int requested_map = atomic_exchange_explicit(&g_cv_warp_map, 0, memory_order_acq_rel);
    if (requested_map <= 0)
        return 0;
    if (episode)
        *episode = atomic_load_explicit(&g_cv_warp_epsd, memory_order_acquire);
    if (map)
        *map = requested_map;
    return 1;
}

void I_RequestRackCheat(int request)
{
    atomic_store_explicit(&g_cv_cheat_request, request, memory_order_release);
}

int I_TakeRackCheat(void)
{
    return atomic_exchange_explicit(&g_cv_cheat_request, 0, memory_order_acq_rel);
}

void I_RequestRackSave(void)
{
    atomic_store_explicit(&g_cv_save_request, 1, memory_order_release);
}

int I_TakeRackSaveRequest(void)
{
    return atomic_exchange_explicit(&g_cv_save_request, 0, memory_order_acq_rel);
}

void I_MarkRackFrameDirty(void)
{
    atomic_store_explicit(&doom_dirty_frame, 1, memory_order_release);
}

int I_TakeRackFrameDirty(void)
{
    return atomic_exchange_explicit(&doom_dirty_frame, 0, memory_order_acq_rel);
}

void I_RequestDoomExit(void)
{
    atomic_store_explicit(&doom_exit_requested, 1, memory_order_release);
}

void I_ClearDoomExitRequest(void)
{
    atomic_store_explicit(&doom_exit_requested, 0, memory_order_release);
}

int I_DoomExitRequested(void)
{
    return atomic_load_explicit(&doom_exit_requested, memory_order_acquire);
}

void I_SetPalette(byte* palette)
{
    memcpy(active_palette, palette, 768);
}

int I_GetPaletteIndex(int r, int g, int b)
{
    return 0;
}

void I_UpdateNoBlit(void)
{
}

void I_FinishUpdate(void)
{
    if (doom_rgba_buffer && I_VideoBuffer)
    {
		pthread_mutex_lock(&doom_rgba_mutex);
        for (int i = 0; i < 320 * 200; ++i)
        {
            byte pixel = I_VideoBuffer[i];
            doom_rgba_buffer[i * 4 + 0] = active_palette[pixel * 3 + 0]; // R
            doom_rgba_buffer[i * 4 + 1] = active_palette[pixel * 3 + 1]; // G
            doom_rgba_buffer[i * 4 + 2] = active_palette[pixel * 3 + 2]; // B
            doom_rgba_buffer[i * 4 + 3] = 255;                          // A
        }
		pthread_mutex_unlock(&doom_rgba_mutex);
        I_MarkRackFrameDirty();
    }
}

void I_SetTargetRGBA(uint8_t *buffer)
{
	pthread_mutex_lock(&doom_rgba_mutex);
    doom_rgba_buffer = buffer;
	pthread_mutex_unlock(&doom_rgba_mutex);
}

void I_CopyTargetRGBA(uint8_t *buffer)
{
    if (buffer == NULL)
    {
        return;
    }

    pthread_mutex_lock(&doom_rgba_mutex);
    if (doom_rgba_buffer != NULL)
    {
        memcpy(buffer, doom_rgba_buffer, 320 * 200 * 4);
    }
    pthread_mutex_unlock(&doom_rgba_mutex);
}

void I_ReadScreen(pixel_t* scr)
{
    if (scr && I_VideoBuffer)
    {
        memcpy(scr, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT * sizeof(pixel_t));
    }
}

void I_BeginRead(void)
{
}

void I_SetWindowTitle(char *title)
{
}

void I_CheckIsScreensaver(void)
{
}

void I_SetGrabMouseCallback(grabmouse_callback_t func)
{
}

void I_DisplayFPSDots(boolean dots_on)
{
}

void I_BindVideoVariables(void)
{
}

void I_InitWindowTitle(void)
{
}

void I_InitWindowIcon(void)
{
}

void I_StartFrame(void)
{
}

void I_StartTic(void)
{
}

void I_EnableLoadingDisk(int xoffs, int yoffs)
{
}
