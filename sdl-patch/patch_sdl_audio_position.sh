#!/bin/bash
# patch_sdl_audio_position.sh - add true pending-playback-frames query + flush
# to SDL 3.4.8 audio backends (PipeWire, PulseAudio, ALSA; WASAPI stub).
#
# Why: PurePlayer's Linux port drives its audio-master clock from an
# estimate (samples_written - queued - guessed device buffer). The Windows
# original uses waveOutGetPosition(), i.e. the real hardware position. The
# estimate ignores PipeWire/Pulse/ALSA server-side latency (20..100 ms+),
# so the clock runs ahead -> video leads -> audible audio lag.
#
# What: new impl entry point
#   bool (*GetDevicePending)(SDL_AudioDevice *d, Uint64 *pending_frames);
# Returns the number of frames written to the device that have NOT been
# heard yet (queued + buffered + graph/device latency), plus a public API:
#   SDL_GetAudioDevicePendingFrames(SDL_AudioStream*, Uint64*)  (query)
#   SDL_FlushAudioStreamDevice(SDL_AudioStream*)                 (drop pending)
# mirroring waveOutGetPosition/waveOutReset semantics.
set -euo pipefail
SDL="${1:-${SDL_ROOT:-../..//src-cache/SDL}}"
# ^ pass the SDL source root as arg 1 (or set SDL_ROOT); default assumes this
# tree sits next to src-cache/ like the original layout
[ -d "$SDL" ] || { echo "usage: $0 <SDL-source-root>  (SDL 3.4.8 tree to patch)"; exit 1; }
cd "$SDL"

# ---------------------------------------------------------------- 1. impl struct
python3 - <<'EOF'
import re
f = 'src/audio/SDL_sysaudio.h'
s = open(f).read()

old = """    void (*FlushRecording)(SDL_AudioDevice *device);
    void (*CloseDevice)(SDL_AudioDevice *device);"""
new = """    void (*FlushRecording)(SDL_AudioDevice *device);
    bool (*GetDevicePending)(SDL_AudioDevice *device, Uint64 *pending_frames);  // PurePlayer sync: frames submitted but not yet audible (waveOutGetPosition equivalent)
    void (*CloseDevice)(SDL_AudioDevice *device);"""
assert old in s
s = s.replace(old, new, 1)
open(f, 'w').write(s)
print('OK sysaudio.h impl struct')
EOF

# ------------------------------------------------- 2. public API + core plumbing
python3 - <<'EOF'
f = 'include/SDL3/SDL_audio.h'
s = open(f).read()

anchor = "extern SDL_DECLSPEC bool SDLCALL SDL_BindAudioStream(SDL_AudioDeviceID devid, SDL_AudioStream *stream);"
assert anchor in s, "bind anchor"
api = anchor + """

/**
 * Query the number of audio frames that have been submitted to the device
 * bound to `stream` but have not been heard by the listener yet. This
 * includes data still queued in the stream plus all device/server-side
 * latency (PipeWire/PulseAudio graph latency, ALSA ring buffer, ...).
 *
 * Returns true on success: the clock in a media player can subtract
 * `pending_frames` from the total submitted frames to get the true audible
 * position, sample-accurate, including every buffering stage.
 *
 * (PurePlayer addition; used for the audio-master sync clock.)
 */
extern SDL_DECLSPEC bool SDLCALL SDL_GetAudioStreamPendingFrames(SDL_AudioStream *stream, Uint64 *pending_frames);

/**
 * Drop everything currently pending on the device bound to `stream`
 * (queued stream data + device-side buffered audio), like waveOutReset().
 * Does NOT clear the stream's own input queue - flush that with
 * SDL_ClearAudioStream() if needed.
 *
 * (PurePlayer addition; used when seeking to kill stale audio.)
 */
extern SDL_DECLSPEC bool SDLCALL SDL_FlushAudioStreamDevice(SDL_AudioStream *stream);"""
s = s.replace(anchor, api, 1)
open(f, 'w').write(s)
print('OK SDL_audio.h API')
EOF

python3 - <<'EOF'
f = 'src/audio/SDL_audio.c'
s = open(f).read()

# default stub in FILL_STUB list
old = """    FILL_STUB(FlushRecording);
    FILL_STUB(CloseDevice);"""
new = """    FILL_STUB(FlushRecording);
    FILL_STUB(GetDevicePending);
    FILL_STUB(CloseDevice);"""
assert old in s
s = s.replace(old, new, 1)

# default impl next to the other stubs
anchor = "static void SDL_AudioCloseDevice_Default(SDL_AudioDevice *device) { /* no-op. */ }"
assert anchor in s
stub = anchor + """

static bool SDL_AudioGetDevicePending_Default(SDL_AudioDevice *device, Uint64 *pending_frames)
{
    // No timing information available from this backend: report the whole
    // device buffer, which is the most conservative estimate.
    if (!pending_frames) {
        return SDL_InvalidParamError("pending_frames");
    }
    *pending_frames = (Uint64)device->sample_frames;
    return true;
}"""
s = s.replace(anchor, stub, 1)

# public entry points, placed right after SDL_GetAudioStreamDevice
defanchor = """SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *stream)
{"""
assert defanchor in s
pub = """SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *stream)
{"""
body_close = """    return 0;
}
"""
# find the real end of the function: search from defanchor for the next "\n}\n"
idx = s.find(defanchor)
end = s.find("\n}\n", idx)
assert end > 0
insert_at = end + len("\n}\n")
pub = """
/* PurePlayer sync extension: true pending playback frames + flush. */
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
    if (!device) {
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
    if (!device) {
        return SDL_SetError("Audio stream is not bound to a device");
    }
    // backend drop of device-side audio
    if (device->FlushPending) {
        device->FlushPending(device);
    }
    return true;
}
"""
s = s[:insert_at] + pub + s[insert_at:]
open(f, 'w').write(s)
print('OK SDL_audio.c core')
EOF
echo "part 1 done"
