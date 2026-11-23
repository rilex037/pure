#!/bin/bash
# patch_player_sync.sh - fix the Linux audio-lag in the PurePlayer port.
#
# Bug: the SDL port guessed the audible position as
#   samples_written - queued - dev_frames(guess)
# ignoring the PipeWire/Pulse/ALSA server-side latency (20..100 ms+), so the
# audio-master clock ran AHEAD of what was audible -> video led, audio lagged.
#
# Fix (exact waveOutGetPosition/waveOutReset semantics from the proven Windows
# engine, via the patched SDL):
#   audible_frames = samples_written - SDL_GetAudioStreamPendingFrames()
#   (true pending: stream queue + device/server latency, PipeWire-first)
# and audio_reset() additionally flushes the device, so stale pre-seek audio
# is dropped instead of playing out.
set -euo pipefail
SRC="${1:-${PLAYER_SRC:-$(dirname "$0")/../src}}"
# ^ pass the player src dir as arg 1 (or set PLAYER_SRC); default: ../src next to sdl-patch/
[ -d "$SRC" ] || { echo "usage: $0 <player-src-dir>  (dir containing ff.c)"; exit 1; }
cd "$SRC"

python3 - <<'EOF'
f = 'ff.c'
s = open(f).read()
done = 'SDL_GetAudioStreamPendingFrames' in s

if not done:
    # ---- ff_clock(): true audible position ----
    old = """    /* audible position = bytes handed to the device minus the device buffer.
     * The device pulls whole buffers, so the raw value moves in ~20 ms steps;
     * a small filter turns that into a smooth clock. */
    long long qb, wr;
    SDL_LockAudioStream(p->astream);
    qb = SDL_GetAudioStreamQueued(p->astream);
    wr = (long long)p->samples_written;
    SDL_UnlockAudioStream(p->astream);
    if (qb < 0)
        qb = 0;
    double pos = (double)(wr - qb / 4 - p->dev_frames);
    if (pos < 0)
        pos = 0;
    double raw = p->audio_base + pos / (double)p->sample_rate;"""
    new = """    /* audible position = frames handed to the stream minus everything not
     * yet audible, taken from the audio backend's true timing (PipeWire
     * pw_stream_get_time / Pulse latency / ALSA delay; waveOutGetPosition
     * equivalent). Includes all server-side latency, which the old
     * dev_frames guess ignored - that made the clock run ahead and the
     * audio lag behind the video. The device updates it in whole periods,
     * so the small filter below turns that into a smooth clock. */
    unsigned long long wr, pend = 0;
    SDL_LockAudioStream(p->astream);
    wr = p->samples_written;
    SDL_UnlockAudioStream(p->astream);
    if (!SDL_GetAudioStreamPendingFrames(p->astream, &pend) || wr < pend)
        pend = wr;      /* nothing audible yet, or backend without timing */
    double pos = (double)(wr - pend);
    if (pos < 0)
        pos = 0;
    double raw = p->audio_base + pos / (double)p->sample_rate;"""
    assert old in s, 'ff_clock anchor'
    s = s.replace(old, new, 1)

    # ---- audio_reset(): also drop device-side audio ----
    old2 = """        SDL_LockAudioStream(p->astream);
        SDL_ClearAudioStream(p->astream);
        p->samples_written = 0;
        SDL_UnlockAudioStream(p->astream);"""
    new2 = """        SDL_LockAudioStream(p->astream);
        SDL_ClearAudioStream(p->astream);
        p->samples_written = 0;
        SDL_UnlockAudioStream(p->astream);
        SDL_FlushAudioStreamDevice(p->astream);    /* drop device-side audio */"""
    assert old2 in s, 'audio_reset anchor'
    s = s.replace(old2, new2, 1)

    open(f, 'w').write(s)
    print('OK: ff.c sync fix applied (clock + reset flush)')
else:
    print('SKIP: ff.c already patched')
EOF
