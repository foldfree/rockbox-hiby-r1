/***************************************************************************
 * HiBy-specific helpers for the hosted ALSA PCM backend
 ****************************************************************************/

#include "autoconf.h"

#include "pcm-alsa-hiby.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <poll.h>
#include <alsa/asoundlib.h>

#include "panic.h"
#include "logf.h"

static char hiby_pcm_bt_mac[18];

/* Poll thread for Bluetooth ALSA (async callbacks unreliable with bluealsa) */
static pthread_t hiby_pcm_poll_thread;
static volatile bool hiby_pcm_poll_thread_stop = false;
static bool hiby_pcm_poll_thread_running = false;
static bool hiby_pcm_mutex_initialized = false;
static unsigned int hiby_pcm_poll_interval_us = 10000;

static snd_pcm_t *hiby_poll_handle = NULL;
static pthread_mutex_t *hiby_poll_mtx = NULL;

/* PCM pump from pcm-alsa.c */
void pcm_alsa_pump_locked(snd_pcm_t *handle);

void hiby_pcm_set_bt_mac(const char *mac)
{
    size_t i;

    if (!mac || !mac[0])
    {
        hiby_pcm_bt_mac[0] = '\0';
        return;
    }

    for (i = 0; mac[i] != '\0' && i + 1 < sizeof(hiby_pcm_bt_mac); i++)
    {
        char c = mac[i];
        if (c == ':')
            c = '_';
        hiby_pcm_bt_mac[i] = (char)toupper((unsigned char)c);
    }
    hiby_pcm_bt_mac[i] = '\0';
}

const char *hiby_pcm_get_bt_mac(void)
{
    return hiby_pcm_bt_mac[0] ? hiby_pcm_bt_mac : NULL;
}

bool hiby_pcm_is_bluealsa_device(const char *device)
{
    return device && strstr(device, "bluealsa:") != NULL;
}

/* Local (built-in DAC) ALSA device, used as the safe fallback when a
 * Bluetooth sink cannot be opened. Keep in sync with
 * BT_LOCAL_PLAYBACK_DEVICE in apps/hiby_bluetooth.c. */
#define HIBY_LOCAL_PLAYBACK_DEVICE "plughw:0,0"

/* ===== Weak Symbol Overrides ===== */

/* Recovery when snd_pcm_open() fails. A Bluetooth sink is hot-pluggable
 * and can disappear between routing and open (e.g. while bluealsa
 * re-negotiates a codec), so a failed open must NOT be fatal: fall back
 * to the local DAC. For the local device itself there is nothing to fall
 * back to, so return NULL and let the caller panic (genuine hardware
 * fault). Returning the same device would loop. */
const char *pcm_alsa_open_fallback(const char *device, int err)
{
    (void)err;

    if (hiby_pcm_is_bluealsa_device(device))
        return HIBY_LOCAL_PLAYBACK_DEVICE;

    return NULL;
}

/* Adjust buffer sizes for Bluetooth devices */
void pcm_alsa_adjust_buffering(snd_pcm_sframes_t *period_size_ptr,
                                snd_pcm_sframes_t *buffer_size_ptr,
                                const char *device)
{
    if (!hiby_pcm_is_bluealsa_device(device))
        return;

    /* BlueALSA needs larger buffers to handle wireless latency */
    *period_size_ptr *= 2;
    if (*period_size_ptr < 2048)
        *period_size_ptr = 2048;
    *buffer_size_ptr = *period_size_ptr * 8;
}

/* Calculate poll interval for custom poll thread */
unsigned int pcm_alsa_calc_poll_interval(unsigned int sample_rate,
                                          snd_pcm_sframes_t period_size_val,
                                          const char *device)
{
    unsigned int interval_us;

    if (sample_rate == 0 || period_size_val <= 0)
        return 10000;

    interval_us = (unsigned int)((period_size_val * 1000000ULL) / sample_rate);

    /* Poll more aggressively for Bluetooth */
    if (hiby_pcm_is_bluealsa_device(device))
        interval_us /= 2;

    if (interval_us < 2000)
        interval_us = 2000;
    if (interval_us > 50000)
        interval_us = 50000;

    hiby_pcm_poll_interval_us = interval_us;
    return interval_us;
}

/* Adjust start threshold for Bluetooth devices */
snd_pcm_uframes_t pcm_alsa_start_threshold(snd_pcm_sframes_t buffer_size_val,
                                            snd_pcm_sframes_t period_size_val,
                                            const char *device)
{
    if (hiby_pcm_is_bluealsa_device(device) && buffer_size_val > period_size_val)
        return (snd_pcm_uframes_t)(buffer_size_val - period_size_val);

    return (snd_pcm_uframes_t)(buffer_size_val / 2);
}

/* Check if PCM params are ready for pumping */
bool pcm_alsa_params_ready(snd_pcm_sframes_t period_size_val,
                           snd_pcm_sframes_t buffer_size_val,
                           void *frames_ptr)
{
    (void)period_size_val;
    (void)buffer_size_val;
    return frames_ptr != NULL;
}

/* Decide whether to keep existing device or reopen */
bool pcm_alsa_keep_device(const char *device, snd_pcm_stream_t mode,
                          const char *current_device, snd_pcm_t *handle
#ifdef HAVE_RECORDING
                          , snd_pcm_stream_t current_mode
#endif
                          )
{
    if (!device || !*device)
        panicf("pcm_alsa_keep_device: Invalid empty ALSA device");

    /* Compare by string, not pointer: the BT route device string lives in a
     * rotating buffer, so the same logical device can have a different
     * pointer between calls. A pointer compare would wrongly keep/repoen. */
    if (!(handle && current_device && strcmp(device, current_device) == 0
#ifdef HAVE_RECORDING
          && current_mode == mode
#endif
         ))
    {
        return false;
    }

    /* Check device is still connected */
    return snd_pcm_state(handle) != SND_PCM_STATE_DISCONNECTED;
}

/* ===== Poll Thread Implementation ===== */

static void hiby_pcm_mutex_init_once(pthread_mutex_t *mtx)
{
    pthread_mutexattr_t attr;

    if (hiby_pcm_mutex_initialized)
        return;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(mtx, &attr);
    pthread_mutexattr_destroy(&attr);
    hiby_pcm_mutex_initialized = true;
}

/* Wait until the PCM can accept more data, then pump. Instead of sleeping a
 * fixed interval and polling blindly, ask ALSA for the device's poll
 * descriptors and wait on them with poll(2) -- we wake exactly when the
 * device is ready (or on a short timeout), which is lower-latency and avoids
 * both busy-spinning and underruns from a too-long sleep.
 *
 * Everything that touches the handle is done under hiby_poll_mtx, and we use
 * a bounded poll timeout so a concurrent route switch (which clears
 * hiby_poll_handle / sets the stop flag) is always noticed within one
 * quantum. If the descriptors can't be fetched (some plugins don't provide
 * usable ones), we fall back to the original fixed-interval sleep+pump. */
static void *hiby_pcm_poll_thread_fn(void *arg)
{
    (void)arg;

    /* Cap the poll wait so stop/route-switch is responsive even if the
     * device never signals readiness. Derived from the computed interval. */
    int max_wait_ms;

    while (!hiby_pcm_poll_thread_stop)
    {
        struct pollfd pfds[8];
        int nfds = 0;
        bool pumped = false;

        max_wait_ms = (int)(hiby_pcm_poll_interval_us / 1000);
        if (max_wait_ms < 2)
            max_wait_ms = 2;
        if (max_wait_ms > 20)
            max_wait_ms = 20;

        if (hiby_poll_handle && hiby_poll_mtx &&
            pthread_mutex_trylock(hiby_poll_mtx) == 0)
        {
            snd_pcm_t *h = hiby_poll_handle; /* may have changed; re-read */

            if (h)
            {
                int count = snd_pcm_poll_descriptors_count(h);
                if (count > 0 && count <= (int)(sizeof(pfds) / sizeof(pfds[0])))
                    nfds = snd_pcm_poll_descriptors(h, pfds, count);

                if (nfds > 0)
                {
                    /* poll() while NOT holding the mutex would race the
                     * handle close, so poll with a short timeout while
                     * holding it. The fds are level-triggered; a brief hold
                     * is fine and keeps the handle alive for the pump. */
                    int rc = poll(pfds, nfds, max_wait_ms);
                    if (rc > 0)
                    {
                        unsigned short revents = 0;
                        if (snd_pcm_poll_descriptors_revents(h, pfds, nfds,
                                                             &revents) == 0 &&
                            (revents & (POLLOUT | POLLERR)))
                        {
                            pcm_alsa_pump_locked(h);
                            pumped = true;
                        }
                    }
                    else if (rc == 0)
                    {
                        /* Timed out with nothing ready: pump anyway so we
                         * recover from XRUN/PREPARED states the descriptors
                         * may not flag. */
                        pcm_alsa_pump_locked(h);
                        pumped = true;
                    }
                }
                else
                {
                    /* No usable descriptors: legacy behaviour. */
                    pcm_alsa_pump_locked(h);
                }
            }

            pthread_mutex_unlock(hiby_poll_mtx);
        }

        /* If we waited inside poll() we've already paced ourselves; only
         * sleep when we didn't (no handle, lock contended, or no fds). */
        if (!pumped)
            usleep(hiby_pcm_poll_interval_us);
    }

    return NULL;
}

static void hiby_pcm_start_poll_thread(snd_pcm_t *handle, pthread_mutex_t *mtx)
{
    int err;

    hiby_pcm_mutex_init_once(mtx);

    if (hiby_pcm_poll_thread_running)
        return;

    hiby_poll_handle = handle;
    hiby_poll_mtx = mtx;
    hiby_pcm_poll_thread_stop = false;

    err = pthread_create(&hiby_pcm_poll_thread, NULL, hiby_pcm_poll_thread_fn, NULL);
    if (err == 0)
        hiby_pcm_poll_thread_running = true;
    else
        panicf("Unable to start ALSA poll thread: %d", err);
}

static void hiby_pcm_stop_poll_thread(void)
{
    if (!hiby_pcm_poll_thread_running)
        return;

    hiby_pcm_poll_thread_stop = true;
    pthread_join(hiby_pcm_poll_thread, NULL);
    hiby_pcm_poll_thread_running = false;
    hiby_poll_handle = NULL;
    hiby_poll_mtx = NULL;
}

/* Device lifecycle hooks */
void pcm_alsa_device_opened(snd_pcm_t *handle, const char *device,
                            snd_pcm_stream_t mode, pthread_mutex_t *mtx)
{
    (void)device;
    (void)mode;

    /* Use poll thread instead of async handler for all devices */
    hiby_pcm_start_poll_thread(handle, mtx);
}

void pcm_alsa_device_closing(snd_pcm_t *handle, const char *device)
{
    (void)handle;
    (void)device;

    hiby_pcm_stop_poll_thread();
}

/* ===== Device Switching API ===== */

/* Forward declare public API from pcm-alsa.c */
void pcm_alsa_set_playback_device(const char *device);

/* Record the desired playback device only. The actual device open/close is
 * driven entirely by the normal sink stop/start path (sink_dma_stop closes
 * the handle, sink_dma_start opens playback_dev), so the caller must bracket
 * a device change with a clean engine stop/start (see the BT route code).
 *
 * This function deliberately does NOT touch the live handle or the pump
 * thread: doing so mid-stream (closing the handle the pump is using, then
 * re-priming concurrently) was the source of the route-switch crashes and
 * stalls. Keep it a pure setter. */
int pcm_alsa_switch_playback_device(const char *device)
{
    if (!device || !*device)
        return -1;

    pcm_alsa_set_playback_device(device);
    return 0;
}
