#!/bin/bash
# patch_sdl_core_units.sh - unit-correct SDL_GetAudioStreamPendingFrames.
#
# Why: the patch-API's core function mixed units:
#  1) queue bytes are stored in the stream's INPUT spec (S16 stereo = 4 B/frame)
#     but were divided by the DEVICE framesize (WASAPI float = 8 B/frame)
#     -> queue counted at half -> clock ran ahead -> audio late (lip-sync).
#  2) backends report pending at the DEVICE rate (48k) while the player
#     counts samples_written at the FILE rate (44.1k) -> ~9% skew.
# Fix: divide queued bytes by input-spec framesize, scale device frames by
# stream_freq/device_freq, take the queue byte count under the stream lock.
set -euo pipefail
SDL="${1:?usage: patch_sdl_core_units.sh <SDL-source-root>}"
cd "$SDL"

python3 - <<'EOF'
def patch(path, old, new, what):
    s = open(path).read()
    if new in s:
        print(f'SKIP (done): {what}')
        return
    assert old in s, f'ANCHOR FAIL {what} in {path}'
    open(path, 'w').write(s.replace(old, new, 1))
    print(f'OK: {what}')

F = 'src/audio/SDL_audio.c'

patch(F,
'''    Uint64 dev_pending = 0;
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
}''',
'''    Uint64 dev_pending = 0;
    if (!device->GetDevicePending(device, &dev_pending)) {
        return false;
    }
    // PurePlayer sync: unit-correct pending count.
    // - backend returns frames at the DEVICE sample rate -> rescale to the
    //   stream's input rate (what the caller counts samples_written in).
    // - queue bytes are stored in the stream's INPUT spec, NOT the device
    //   spec (float32 8 B/frame vs S16 4 B/frame halved the queue before ->
    //   clock ran ahead -> audio lagged).
    double pend = (double)dev_pending;
    if (device->spec.freq > 0) {
        pend *= (double)stream->input_spec.freq / (double)device->spec.freq;
    }
    SDL_LockMutex(stream->lock);
    const size_t queued = SDL_GetAudioQueueQueued(stream->queue);
    SDL_UnlockMutex(stream->lock);
    const Uint64 in_fsize = (Uint64)SDL_AUDIO_FRAMESIZE(stream->input_spec);
    if (queued > 0 && in_fsize > 0) {
        pend += (double)(queued / in_fsize);
    }
    *pending_frames = (Uint64)(pend + 0.5);
    return true;
}''',
'core unit-correct pending frames')

# queue API + stream fields live in audiocvt; core needs the queue decl
s = open(F).read()
if '#include "SDL_audioqueue.h"' not in s:
    s = s.replace('#include "SDL_sysaudio.h"', '#include "SDL_sysaudio.h"\n#include "SDL_audioqueue.h"', 1)
    open(F, 'w').write(s)
    print('OK: queue header include')
else:
    print('SKIP: queue header include')
EOF
echo "core units patch done"
