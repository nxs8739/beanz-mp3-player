/* BEANZ MP3 Player: small frame decoder + ASND streaming, libogc 3.2.1. */
#include <gccore.h>
#include <asndlib.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include "mp3_audio.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#define INPUT_BYTES 16384
#define CHUNK_FRAMES 8
#define MAX_PCM_BYTES (CHUNK_FRAMES * MINIMP3_MAX_SAMPLES_PER_FRAME * 2)
#define SLOT_COUNT 3
#define VOICE 0
/* 300 consecutive queue rejections ~5 seconds of 60 Hz pump calls. */
#define QUEUE_RETRY_LIMIT 300
static FILE *song;
static mp3dec_t decoder;
static unsigned char input[INPUT_BYTES];
static int in_len, at_eof, decoded_eof, running, paused, voice_started;
static int sample_rate;
static unsigned total_seconds;
static unsigned long long decoded_samples;
static unsigned long long completed_samples;
static unsigned long long queued_samples[SLOT_COUNT];
static unsigned char *pcm[SLOT_COUNT];
static int pcm_size[SLOT_COUNT];
static int pcm_busy[SLOT_COUNT];
/* Decoded PCM awaiting ASND acceptance. Do not decode over this slot on busy. */
static int pending_slot = -1;
static unsigned queue_rejections;
static char error_text[96];
static mp3d_sample_t frame_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

/* Count MPEG Layer III audio samples without decoding PCM or changing playback state.
   This handles constant and variable bitrates using each frame's own header. */
static unsigned scan_duration(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    /* Skip an ID3v2 tag before looking for MPEG frames. Album artwork and
       other metadata can contain false MP3 sync patterns; locking onto one
       would make the duration scanner reject the real song sample rate. */
    unsigned char id3[10];
    if (fread(id3, 1, sizeof(id3), f) == sizeof(id3) &&
        memcmp(id3, "ID3", 3) == 0 && id3[3] >= 2 && id3[3] <= 4 &&
        !(id3[6] & 0x80) && !(id3[7] & 0x80) &&
        !(id3[8] & 0x80) && !(id3[9] & 0x80)) {
        unsigned tag_bytes = ((unsigned)id3[6] << 21) |
                             ((unsigned)id3[7] << 14) |
                             ((unsigned)id3[8] << 7) | (unsigned)id3[9];
        long audio_offset = (long)tag_bytes + 10;
        if (id3[3] == 4 && (id3[5] & 0x10)) audio_offset += 10;
        if (fseek(f, audio_offset, SEEK_SET) != 0) {
            fclose(f);
            return 0;
        }
    } else {
        rewind(f);
    }
    unsigned char buf[8192];
    size_t have = 0, pos = 0;
    unsigned long long sample_count = 0;
    int rate = 0, found = 0;
    int eof = 0;
    while (1) {
        if (have - pos < 4 && !eof) {
            if (pos && have > pos) memmove(buf, buf + pos, have - pos);
            have -= pos;
            pos = 0;
            size_t n = fread(buf + have, 1, sizeof(buf) - have, f);
            have += n;
            if (!n) eof = 1;
        }
        if (have - pos < 4) break;
        unsigned h = ((unsigned)buf[pos] << 24) | ((unsigned)buf[pos+1] << 16) |
                     ((unsigned)buf[pos+2] << 8) | buf[pos+3];
        unsigned version = (h >> 19) & 3;
        unsigned layer = (h >> 17) & 3;
        unsigned bitrate_idx = (h >> 12) & 15;
        unsigned rate_idx = (h >> 10) & 3;
        if ((h & 0xffe00000u) != 0xffe00000u || version == 1 ||
            layer != 1 || bitrate_idx == 0 || bitrate_idx == 15 || rate_idx == 3) {
            ++pos;
            continue;
        }
        static const unsigned rates[3] = {44100, 48000, 32000};
        static const unsigned br_mpeg1[14] = {32,40,48,56,64,80,96,112,128,160,192,224,256,320};
        static const unsigned br_mpeg2[14] = {8,16,24,32,40,48,56,64,80,96,112,128,144,160};
        unsigned hz = rates[rate_idx];
        if (version == 2) hz /= 2;
        if (version == 0) hz /= 4;
        unsigned kbps = (version == 3 ? br_mpeg1 : br_mpeg2)[bitrate_idx-1];
        unsigned frame_bytes = ((version == 3 ? 144000u : 72000u) * kbps) / hz + ((h >> 9) & 1);
        if (frame_bytes < 4 || frame_bytes > sizeof(buf)) { ++pos; continue; }
        /* Read the entire frame before counting it. */
        if (have - pos < frame_bytes) {
            if (eof) break;
            if (pos && have > pos) memmove(buf, buf + pos, have - pos);
            have -= pos;
            pos = 0;
            size_t n = fread(buf + have, 1, sizeof(buf) - have, f);
            have += n;
            if (!n) eof = 1;
            continue;
        }
        if (!rate) rate = (int)hz;
        if (rate != (int)hz) { ++pos; continue; }
        sample_count += version == 3 ? 1152u : 576u;
        ++found;
        pos += frame_bytes;
    }
    fclose(f);
    if (!found || !rate) return 0;
    return (unsigned)((sample_count + rate / 2) / rate);
}

const char *mp3_audio_error(void) { return error_text; }
int mp3_audio_active(void) { return running; }
int mp3_audio_paused(void) { return paused; }
int mp3_audio_init(void) {
    int i;
    for (i = 0; i < SLOT_COUNT; ++i) {
        pcm[i] = memalign(32, MAX_PCM_BYTES);
        if (!pcm[i]) { strcpy(error_text, "Cannot allocate PCM buffers"); return 0; }
    }
    ASND_Init();
    ASND_Pause(0);
    return 1;
}
static void consume(int n) {
    if (n <= 0) return;
    if (n < in_len) memmove(input, input + n, (size_t)(in_len - n));
    in_len -= n;
}
static void refill(void) {
    if (!song || at_eof || in_len >= INPUT_BYTES) return;
    size_t n = fread(input + in_len, 1, INPUT_BYTES - in_len, song);
    in_len += (int)n;
    if (!n) at_eof = 1;
}
/* Returns samples per channel, 0 at end of file. PCM is interleaved. */
static int decode_frame(mp3dec_frame_info_t *info) {
    int attempts = 0;
    while (attempts++ < 32) {
        int samples;
        refill();
        if (in_len < 4) { if (at_eof) return 0; continue; }
        memset(info, 0, sizeof(*info));
        samples = mp3dec_decode_frame(&decoder, input, in_len, frame_pcm, info);
        if (info->frame_bytes > 0) {
            consume(info->frame_bytes);
            if (samples > 0) return samples;
            continue;
        }
        /* Incomplete frame near the end of the input window. */
        if (!at_eof && in_len < INPUT_BYTES) {
            int old = in_len;
            refill();
            if (in_len > old) continue;
        }
        if (at_eof && in_len < 4) return 0;
        consume(1); /* skip unrecognized metadata or sync byte */
    }
    return -1; /* bounded work per pump; try again next iteration */
}
static int fill_slot(int slot) {
    int bytes = 0, frames = 0;
    while (frames < CHUNK_FRAMES) {
        mp3dec_frame_info_t info;
        int samples = decode_frame(&info);
        if (samples == -1) break;
        if (samples == 0) { decoded_eof = 1; break; }
        if (info.channels != 1 && info.channels != 2) continue;
        if (!sample_rate) sample_rate = info.hz;
        if (info.hz != sample_rate) { strcpy(error_text, "MP3 sample rate changed"); decoded_eof = 1; break; }
        /* ASND accepts native 16-bit stereo; duplicate mono if needed. */
        for (int i = 0; i < samples; ++i) {
            int16_t l = frame_pcm[i * info.channels];
            int16_t r = frame_pcm[i * info.channels + info.channels - 1];
            /* Explicit big-endian PCM output for the PowerPC/ASND. */
            pcm[slot][bytes++] = (unsigned char)((uint16_t)l >> 8);
            pcm[slot][bytes++] = (unsigned char)l;
            pcm[slot][bytes++] = (unsigned char)((uint16_t)r >> 8);
            pcm[slot][bytes++] = (unsigned char)r;
        }
        decoded_samples += (unsigned)samples;
        ++frames;
    }
    if (!bytes) return 0;
    /* ASND requires 32-byte-aligned size; pad final partial buffer. */
    int padded = (bytes + 31) & ~31;
    memset(pcm[slot] + bytes, 0, (size_t)(padded - bytes));
    pcm_size[slot] = padded;
    queued_samples[slot] = (unsigned long long)bytes / 4;
    return 1;
}
void mp3_audio_stop(void) {
    if (voice_started) ASND_StopVoice(VOICE);
    voice_started = 0;
    if (song) { fclose(song); song = NULL; }
    running = paused = decoded_eof = at_eof = 0;
    in_len = sample_rate = 0;
    decoded_samples = completed_samples = 0;
    total_seconds = 0;
    memset(pcm_busy, 0, sizeof(pcm_busy));
    pending_slot = -1;
    queue_rejections = 0;
}
int mp3_audio_open(const char *path) {
    mp3_audio_stop();
    error_text[0] = 0;
    total_seconds = scan_duration(path);
    song = fopen(path, "rb");
    if (!song) { strcpy(error_text, "Cannot open MP3 file"); return 0; }
    mp3dec_init(&decoder);
    running = 1;
    mp3_audio_pump();
    if (!voice_started && decoded_eof && pending_slot < 0) {
        strcpy(error_text, "No playable MP3 frames found");
        mp3_audio_stop(); return 0;
    }
    return 1;
}
void mp3_audio_pause_toggle(void) {
    if (!running || !voice_started) return;
    paused = !paused;
    ASND_PauseVoice(VOICE, paused);
}
unsigned mp3_audio_total_seconds(void) { return total_seconds; }
unsigned mp3_audio_elapsed_seconds(void) {
    if (!running || !sample_rate) return 0;
    /* ASND voice timer is a playback counter, not a decoding counter. */
    return voice_started ? ASND_GetTimerVoice(VOICE) / 1000 : 0;
}
void mp3_audio_pump(void) {
    if (!running || paused) return;
    /* Retire buffers only when ASND no longer references their addresses. */
    for (int i = 0; i < SLOT_COUNT; ++i)
        if (pcm_busy[i] && !ASND_TestPointer(VOICE, pcm[i])) {
            pcm_busy[i] = 0;
            completed_samples += queued_samples[i];
        }
    /* The last decoded chunk may still be pending after a temporary ASND
       rejection. Never treat EOF as finished until it has been accepted. */
    if (pending_slot < 0) {
        if (decoded_eof) {
            int busy = 0;
            for (int i = 0; i < SLOT_COUNT; ++i) busy += pcm_busy[i];
            if (!busy) mp3_audio_stop();
            return;
        }
        if (voice_started && !ASND_TestVoiceBufferReady(VOICE)) return;
        int slot = -1;
        for (int i = 0; i < SLOT_COUNT; ++i)
            if (!pcm_busy[i]) { slot = i; break; }
        if (slot < 0 || !fill_slot(slot)) return;
        if (!sample_rate) return;
        pending_slot = slot;
    }
    /* ASND can reject a voice update even after reporting buffer readiness.
       Hold the exact same decoded PCM and retry instead of dropping it and
       mistakenly returning to the file browser. */
    if (voice_started && !ASND_TestVoiceBufferReady(VOICE)) return;
    int slot = pending_slot;
    s32 rc;
    if (!voice_started) {
        rc = ASND_SetVoice(VOICE, VOICE_STEREO_16BIT, sample_rate, 0,
                           pcm[slot], pcm_size[slot], 255, 255, NULL);
        if (rc == SND_OK) voice_started = 1;
    } else {
        rc = ASND_AddVoice(VOICE, pcm[slot], pcm_size[slot]);
    }
    if (rc == SND_OK) {
        pcm_busy[slot] = 1;
        pending_slot = -1;
        queue_rejections = 0;
    } else if (++queue_rejections >= QUEUE_RETRY_LIMIT) {
        strcpy(error_text, "ASND queue failed repeatedly");
        mp3_audio_stop();
    }
}
void mp3_audio_shutdown(void) {
    mp3_audio_stop();
    ASND_End();
    for (int i = 0; i < SLOT_COUNT; ++i) { free(pcm[i]); pcm[i] = NULL; }
}
