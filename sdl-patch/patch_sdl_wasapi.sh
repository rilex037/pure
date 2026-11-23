#!/bin/bash
# patch_sdl_wasapi.sh - add the true pending-playback-frames query + flush to
# the SDL 3.4.8 WASAPI backend (Windows). Windows counterpart of
# patch_sdl_audio_position.sh / patch_sdl_part2.sh (PipeWire/Pulse/ALSA).
#
# Why: without this, WASAPI used the generic default (one device buffer as a
# guess). The audio-master clock then ran ahead of what was audible -> video
# led, audio lagged - the same bug class the Linux port had.
#
# What: GetDevicePending = GetCurrentPadding() (frames written to the WASAPI
# endpoint but not yet heard - exact waveOutGetPosition equivalent),
# FlushPending = Stop + SetBufferStopped... i.e. IAudioClient_Stop then Reset(),
# the waveOutReset equivalent. Signature/semantics identical to the Linux
# backends' patch.
set -euo pipefail
SDL="${1:-${SDL_ROOT:-../..//src-cache/SDL}}"
[ -d "$SDL" ] || { echo "usage: $0 <SDL-source-root>"; exit 1; }
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

F = 'src/audio/wasapi/SDL_wasapi.c'

# 1) implementations, placed right after WASAPI_FlushRecording (its final
#    closing brace + blank line anchor) so they sit with the other period ops.
patch(F,
"""static void WASAPI_CloseDevice(SDL_AudioDevice *device)
{""",
"""static bool WASAPI_GetDevicePending(SDL_AudioDevice *device, Uint64 *pending_frames)
{
    // PurePlayer sync: frames submitted to the endpoint but not yet
    // audible = GetCurrentPadding(). Exact waveOutGetPosition equivalent:
    // the mixer queue the clock must subtract, including all device latency.
    if (!pending_frames) {
        return false;  // caller checks params; core already validated
    }
    if (!device->hidden || !device->hidden->client || device->recording) {
        *pending_frames = 0;  // not activated yet (or capture): nothing audible is queued
        return true;
    }
    UINT32 padding = 0;
    if (WasapiFailed(device, IAudioClient_GetCurrentPadding(device->hidden->client, &padding))) {
        return false;  // device lost; caller falls back to device->sample_frames
    }
    *pending_frames = (Uint64)padding;
    return true;
}

static void WASAPI_FlushPending(SDL_AudioDevice *device)
{
    // PurePlayer sync: waveOutReset equivalent - drop everything still
    // queued in the endpoint so stale pre-seek audio never plays out.
    // Stop drains nothing (it just halts new processing); Stop + Reset
    // together clear the shared buffer, then Start resumes immediately.
    if (!device->hidden || !device->hidden->client || device->recording) {
        return;
    }
    if (!WasapiFailed(device, IAudioClient_Stop(device->hidden->client))) {
        WasapiFailed(device, IAudioClient_Reset(device->hidden->client));
        WasapiFailed(device, IAudioClient_Start(device->hidden->client));
    }
}

static void WASAPI_CloseDevice(SDL_AudioDevice *device)
{""",
'WASAPI GetDevicePending + FlushPending impls')

# 2) registration
patch(F,
"""    impl->FlushRecording = WASAPI_FlushRecording;
    impl->CloseDevice = WASAPI_CloseDevice;""",
"""    impl->FlushRecording = WASAPI_FlushRecording;
    impl->GetDevicePending = WASAPI_GetDevicePending;   // PurePlayer sync
    impl->FlushPending = WASAPI_FlushPending;           // PurePlayer sync
    impl->CloseDevice = WASAPI_CloseDevice;""",
'WASAPI impl registration')
EOF
echo "wasapi patch done"
