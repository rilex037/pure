#!/bin/bash
# patch_sdl_part2.sh - complete the SDL audio-position patch:
# FlushPending impl field, public API decl, core plumbing, and the
# PipeWire / PulseAudio / ALSA backend implementations.
set -euo pipefail
SDL="${1:-${SDL_ROOT:-../..//src-cache/SDL}}"
# ^ pass the SDL source root as arg 1 (or set SDL_ROOT); part1 must run first
[ -d "$SDL" ] || { echo "usage: $0 <SDL-source-root>  (after patch_sdl_audio_position.sh)"; exit 1; }
cd "$SDL"

python3 - <<'EOF'
def patch(path, old, new, what):
    s = open(path).read()
    if new.strip() and new in s:
        print(f'SKIP (done): {what}')
        return
    assert old in s, f'ANCHOR FAIL {what} in {path}'
    open(path, 'w').write(s.replace(old, new, 1))
    print(f'OK: {what}')

# 1) impl struct: FlushPending field (GetDevicePending added by part 1)
patch('src/audio/SDL_sysaudio.h',
"""    bool (*GetDevicePending)(SDL_AudioDevice *device, Uint64 *pending_frames);  // PurePlayer sync: frames submitted but not yet audible (waveOutGetPosition equivalent)""",
"""    bool (*GetDevicePending)(SDL_AudioDevice *device, Uint64 *pending_frames);  // PurePlayer sync: frames submitted but not yet audible (waveOutGetPosition equivalent)
    void (*FlushPending)(SDL_AudioDevice *device);  // PurePlayer sync: drop device-side pending audio (waveOutReset equivalent)""",
'impl FlushPending field')

# 2) public API decls
patch('include/SDL3/SDL_audio.h',
"""extern SDL_DECLSPEC bool SDLCALL SDL_BindAudioStream(SDL_AudioDeviceID devid, SDL_AudioStream *stream);""",
"""extern SDL_DECLSPEC bool SDLCALL SDL_BindAudioStream(SDL_AudioDeviceID devid, SDL_AudioStream *stream);

/**
 * Query the number of audio frames submitted to the device bound to `stream`
 * that have not been heard yet: everything still queued in the stream plus
 * all device / server-side latency (PipeWire/PulseAudio graph latency, ALSA
 * ring buffer, ...). Sample-accurate audible-position support, the
 * waveOutGetPosition() equivalent. (PurePlayer sync extension.)
 */
extern SDL_DECLSPEC bool SDLCALL SDL_GetAudioStreamPendingFrames(SDL_AudioStream *stream, Uint64 *pending_frames);

/**
 * Drop everything currently pending on the device bound to `stream`,
 * like waveOutReset(). Does not clear the stream's own input queue.
 * (PurePlayer sync extension.)
 */
extern SDL_DECLSPEC bool SDLCALL SDL_FlushAudioStreamDevice(SDL_AudioStream *stream);""",
'public API decls')

# 3) core: FILL_STUB entries
patch('src/audio/SDL_audio.c',
"""    FILL_STUB(FlushRecording);
    FILL_STUB(GetDevicePending);
    FILL_STUB(CloseDevice);""",
"""    FILL_STUB(FlushRecording);
    FILL_STUB(GetDevicePending);
    FILL_STUB(FlushPending);
    FILL_STUB(CloseDevice);""",
'FILL_STUB')

# 4) core: default stubs
patch('src/audio/SDL_audio.c',
"""static void SDL_AudioCloseDevice_Default(SDL_AudioDevice *device) { /* no-op. */ }""",
"""static void SDL_AudioCloseDevice_Default(SDL_AudioDevice *device) { /* no-op. */ }

static bool SDL_AudioGetDevicePending_Default(SDL_AudioDevice *device, Uint64 *pending_frames)
{
    // Backend without timing information: report one device buffer,
    // the most conservative estimate.
    if (!pending_frames) {
        return SDL_InvalidParamError("pending_frames");
    }
    *pending_frames = (Uint64)device->sample_frames;
    return true;
}

static void SDL_AudioFlushPending_Default(SDL_AudioDevice *device) { /* no-op. */ }""",
'default stubs')

# 5) core: public functions, before the playback thread section
patch('src/audio/SDL_audio.c',
"""// Playback device thread. This is split into chunks, so backends that need to control this directly can use the pieces they need without duplicating effort.""",
"""// PurePlayer sync extension: true pending playback frames + flush.

bool SDL_GetAudioStreamPendingFrames(SDL_AudioStream *stream, Uint64 *pending_frames)
{
    if (!pending_frames) {
        return SDL_InvalidParamError("pending_frames");
    }
    *pending_frames = 0;
    if (!stream || !stream->bound_device) {
        return SDL_InvalidParamError("stream");
    }
    SDL_AudioDevice *device = stream->bound_device->physical_device;
    if (!device || !device->hidden) {
        return SDL_SetError("Audio stream is not bound to a device");
    }
    Uint64 dev_pending = 0;
    if (!device->GetDevicePending(device, &dev_pending)) {
        return false;
    }
    // frames still sitting in the stream's own conversion queue
    const int queued = SDL_GetAudioStreamQueued(stream);
    if (queued > 0) {
        dev_pending += (Uint64)(queued / SDL_AUDIO_FRAMESIZE(device->spec));
    }
    *pending_frames = dev_pending;
    return true;
}

bool SDL_FlushAudioStreamDevice(SDL_AudioStream *stream)
{
    if (!stream || !stream->bound_device) {
        return SDL_InvalidParamError("stream");
    }
    SDL_AudioDevice *device = stream->bound_device->physical_device;
    if (!device || !device->hidden) {
        return SDL_SetError("Audio stream is not bound to a device");
    }
    device->FlushPending(device);
    return true;
}

// Playback device thread. This is split into chunks, so backends that need to control this directly can use the pieces they need without duplicating effort.""",
'core public functions')
EOF

# ---------------- PipeWire backend ----------------
python3 - <<'EOF'
f = 'src/audio/pipewire/SDL_pipewire.c'
s = open(f).read()
def patch(old, new, what):
    global s
    if new in s and new.strip():
        print(f'SKIP (done): {what}'); return
    assert old in s, f'ANCHOR FAIL {what}'
    s = s.replace(old, new, 1)
    print(f'OK: {what}')

# function pointers
patch("""static int (*PIPEWIRE_pw_stream_queue_buffer)(struct pw_stream *, struct pw_buffer *);""",
"""static int (*PIPEWIRE_pw_stream_queue_buffer)(struct pw_stream *, struct pw_buffer *);
static int (*PIPEWIRE_pw_stream_get_time_n)(struct pw_stream *, struct pw_time *, size_t);
static int (*PIPEWIRE_pw_stream_flush)(struct pw_stream *, bool);""",
'pw fn ptrs')

# symbol loading
patch("""    SDL_PIPEWIRE_SYM(pw_stream_queue_buffer);""",
"""    SDL_PIPEWIRE_SYM(pw_stream_queue_buffer);
    SDL_PIPEWIRE_SYM(pw_stream_get_time_n);
    SDL_PIPEWIRE_SYM(pw_stream_flush);""",
'pw syms')

# implementations, inserted before PIPEWIRE_PREFERRED_Init
patch("""static bool PIPEWIRE_PREFERRED_Init(SDL_AudioDriverImpl *impl)""",
"""// PurePlayer sync: frames submitted but not yet audible =
// queued + resampler buffered + graph/device latency (pw_time docs),
// the waveOutGetPosition() equivalent. rate is a fraction (1/samplerate).
static bool PIPEWIRE_GetDevicePending(SDL_AudioDevice *device, Uint64 *pending_frames)
{
    struct SDL_PrivateAudioData *priv = device->hidden;
    struct pw_time t;
    if (!priv || !priv->stream) {
        *pending_frames = 0;
        return true;
    }
    SDL_zero(t);
    PIPEWIRE_pw_thread_loop_lock(priv->loop);
    const int rc = PIPEWIRE_pw_stream_get_time_n(priv->stream, &t, sizeof(t));
    PIPEWIRE_pw_thread_loop_unlock(priv->loop);
    if (rc != 0 || t.rate.num == 0 || t.rate.denom == 0) {
        *pending_frames = (Uint64)device->sample_frames;   // conservative fallback
        return true;
    }
    double s = ((double)t.queued + (double)t.buffered) / (double)device->spec.freq;
    s += (double)t.delay * (double)t.rate.num / (double)t.rate.denom;
    if (s < 0.0) {
        s = 0.0;
    }
    *pending_frames = (Uint64)(s * (double)device->spec.freq + 0.5);
    return true;
}

// PurePlayer sync: drop everything buffered server-side.
static void PIPEWIRE_FlushPending(SDL_AudioDevice *device)
{
    struct SDL_PrivateAudioData *priv = device->hidden;
    if (priv && priv->stream) {
        PIPEWIRE_pw_thread_loop_lock(priv->loop);
        PIPEWIRE_pw_stream_flush(priv->stream, false);
        PIPEWIRE_pw_thread_loop_unlock(priv->loop);
    }
}

static bool PIPEWIRE_PREFERRED_Init(SDL_AudioDriverImpl *impl)""",
'pw impls')

# registration
patch("""    impl->FlushRecording = PIPEWIRE_FlushRecording;""",
"""    impl->FlushRecording = PIPEWIRE_FlushRecording;
    impl->GetDevicePending = PIPEWIRE_GetDevicePending;
    impl->FlushPending = PIPEWIRE_FlushPending;""",
'pw registration')
open(f, 'w').write(s)
EOF

# ---------------- PulseAudio backend ----------------
python3 - <<'EOF'
f = 'src/audio/pulseaudio/SDL_pulseaudio.c'
s = open(f).read()
def patch(old, new, what):
    global s
    if new in s and new.strip():
        print(f'SKIP (done): {what}'); return
    assert old in s, f'ANCHOR FAIL {what}'
    s = s.replace(old, new, 1)
    print(f'OK: {what}')

# symbol pointer for pa_stream_get_latency (pa_stream_flush already loaded)
patch("""static pa_operation *(*PULSEAUDIO_pa_stream_flush)(pa_stream *,
                                                   pa_stream_success_cb_t, void *);""",
"""static pa_operation *(*PULSEAUDIO_pa_stream_flush)(pa_stream *,
                                                   pa_stream_success_cb_t, void *);
static int (*PULSEAUDIO_pa_stream_get_latency)(pa_stream *, pa_usec_t *, int *);""",
'pa fn ptr')

patch("""    SDL_PULSEAUDIO_SYM(pa_stream_flush);""",
"""    SDL_PULSEAUDIO_SYM(pa_stream_flush);
    SDL_PULSEAUDIO_SYM(pa_stream_get_latency);""",
'pa sym')

# find FlushRecording impl for anchor + registration anchor
import re
m = re.search(r'impl->FlushRecording = PULSEAUDIO_FlushRecording;', s)
assert m, 'ANCHOR FAIL pa registration'
patch("""    impl->FlushRecording = PULSEAUDIO_FlushRecording;""",
"""    impl->FlushRecording = PULSEAUDIO_FlushRecording;
    impl->GetDevicePending = PULSEAUDIO_GetDevicePending;
    impl->FlushPending = PULSEAUDIO_FlushPending;""",
'pa registration')

# implementations before OpenDevice
patch("""static bool PULSEAUDIO_OpenDevice(SDL_AudioDevice *device)""",
"""// PurePlayer sync: pa_stream_get_latency = time a frame written now
// takes until it is heard: the whole server+sink pipeline. waveOutGetPosition
// equivalent. Uses the interpolated timing path when available.
static bool PULSEAUDIO_GetDevicePending(SDL_AudioDevice *device, Uint64 *pending_frames)
{
    struct SDL_PrivateAudioData *h = device->hidden;
    if (!h || !h->stream) {
        *pending_frames = 0;
        return true;
    }
    pa_usec_t usec = 0;
    int neg = 0;
    PULSEAUDIO_pa_threaded_mainloop_lock(pulseaudio_threaded_mainloop);
    const int rc = PULSEAUDIO_pa_stream_get_latency(h->stream, &usec, &neg);
    PULSEAUDIO_pa_threaded_mainloop_unlock(pulseaudio_threaded_mainloop);
    if (rc != 0 || neg) {
        *pending_frames = (Uint64)device->sample_frames;   // no timing data yet
        return true;
    }
    double sec = (double)usec / 1000000.0;
    if (sec < 0.0) {
        sec = 0.0;
    }
    *pending_frames = (Uint64)(sec * (double)device->spec.freq + 0.5);
    return true;
}

// PurePlayer sync: drop server-side buffered audio (waveOutReset).
static void PULSEAUDIO_FlushPending(SDL_AudioDevice *device)
{
    struct SDL_PrivateAudioData *h = device->hidden;
    if (!h || !h->stream) {
        return;
    }
    PULSEAUDIO_pa_threaded_mainloop_lock(pulseaudio_threaded_mainloop);
    pa_operation *op = PULSEAUDIO_pa_stream_flush(h->stream, NULL, NULL);
    if (op) {
        while (PULSEAUDIO_pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
            PULSEAUDIO_pa_threaded_mainloop_wait(pulseaudio_threaded_mainloop);
        }
        PULSEAUDIO_pa_operation_unref(op);
    }
    PULSEAUDIO_pa_threaded_mainloop_unlock(pulseaudio_threaded_mainloop);
}

static bool PULSEAUDIO_OpenDevice(SDL_AudioDevice *device)""",
'pa impls')
open(f, 'w').write(s)
EOF

# ---------------- ALSA backend ----------------
python3 - <<'EOF'
f = 'src/audio/alsa/SDL_alsa_audio.c'
s = open(f).read()
def patch(old, new, what):
    global s
    if new in s and new.strip():
        print(f'SKIP (done): {what}'); return
    assert old in s, f'ANCHOR FAIL {what}'
    s = s.replace(old, new, 1)
    print(f'OK: {what}')

# snd_pcm_delay / snd_pcm_drop if not present
if 'ALSA_snd_pcm_delay' not in s:
    patch("""static snd_pcm_sframes_t (*ALSA_snd_pcm_avail)(snd_pcm_t *);""",
"""static snd_pcm_sframes_t (*ALSA_snd_pcm_avail)(snd_pcm_t *);
static int (*ALSA_snd_pcm_delay)(snd_pcm_t *, snd_pcm_sframes_t *);
static int (*ALSA_snd_pcm_drop)(snd_pcm_t *);
static int (*ALSA_snd_pcm_prepare)(snd_pcm_t *);""",
    'alsa fn ptrs')
    patch("""    SDL_ALSA_SYM(snd_pcm_avail);""",
"""    SDL_ALSA_SYM(snd_pcm_avail);
    SDL_ALSA_SYM(snd_pcm_delay);
    SDL_ALSA_SYM(snd_pcm_drop);
    SDL_ALSA_SYM(snd_pcm_prepare);""",
    'alsa syms')

# implementations + registration
patch("""static bool ALSA_WaitDevice(SDL_AudioDevice *device)""",
"""// PurePlayer sync: frames in the ALSA ring buffer not yet played.
static bool ALSA_GetDevicePending(SDL_AudioDevice *device, Uint64 *pending_frames)
{
    if (!device->hidden || !device->hidden->pcm) {
        *pending_frames = 0;
        return true;
    }
    snd_pcm_sframes_t delay = 0;
    if (ALSA_snd_pcm_delay(device->hidden->pcm, &delay) < 0 || delay < 0) {
        *pending_frames = (Uint64)device->sample_frames;   // conservative fallback
        return true;
    }
    *pending_frames = (Uint64)delay;
    return true;
}

// PurePlayer sync: drop the ring buffer contents.
static void ALSA_FlushPending(SDL_AudioDevice *device)
{
    if (device->hidden && device->hidden->pcm) {
        ALSA_snd_pcm_drop(device->hidden->pcm);
        ALSA_snd_pcm_prepare(device->hidden->pcm);
    }
}

static bool ALSA_WaitDevice(SDL_AudioDevice *device)""",
'alsa impls')

import re
m = re.search(r'impl->WaitDevice = ALSA_WaitDevice;', s)
assert m, 'ANCHOR FAIL alsa registration'
patch("""    impl->WaitDevice = ALSA_WaitDevice;""",
"""    impl->WaitDevice = ALSA_WaitDevice;
    impl->GetDevicePending = ALSA_GetDevicePending;
    impl->FlushPending = ALSA_FlushPending;""",
'alsa registration')
open(f, 'w').write(s)
EOF
echo "SDL patch part 2 complete."
