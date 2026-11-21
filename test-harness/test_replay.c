/* Host regression harness: only mocks GameCube ASND; audio engine unchanged.
 * Supply a local test MP3 at runtime. No MP3 is part of this source archive.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "stubs/asndlib.h"
#include "mp3_audio.h"

enum { NORMAL, ONCE_BUSY, ONCE_INVALID, THREE_BUSY, ONCE_SET_INVALID, ALWAYS_BUSY };
static int mode, failed_adds, failed_sets, queue_count, paused;
static int sets, adds, stops, attempts, rejections;
static void *queue[8];

void ASND_Init(void) {}
void ASND_Pause(int p) { (void)p; }
void ASND_End(void) {}
void ASND_StopVoice(int voice) {
    (void)voice;
    queue_count = paused = 0;
    ++stops;
}
void ASND_PauseVoice(int voice, int p) { (void)voice; paused=p; }
int ASND_SetVoice(int voice, int fmt, int hz, int delay,
                  void *ptr, int size, int l, int r, void *cb) {
    (void)voice; (void)fmt; (void)hz; (void)delay;
    (void)size; (void)l; (void)r; (void)cb;
    ++attempts;
    if (mode == ONCE_SET_INVALID && !failed_sets++) { ++rejections; return SND_INVALID; }
    if (queue_count >= 8) return SND_BUSY;
    queue[queue_count++]=ptr;
    ++sets;
    return SND_OK;
}
int ASND_AddVoice(int voice, void *ptr, int size) {
    (void)voice; (void)size;
    ++attempts;
    if ((mode == ONCE_BUSY && failed_adds < 1) ||
        (mode == ONCE_INVALID && failed_adds < 1) ||
        (mode == THREE_BUSY && failed_adds < 3) ||
        (mode == ALWAYS_BUSY)) {
        ++failed_adds; ++rejections;
        return mode == ONCE_INVALID ? SND_INVALID : SND_BUSY;
    }
    if (queue_count >= 8) return SND_BUSY;
    queue[queue_count++]=ptr;
    ++adds;
    return SND_OK;
}
int ASND_TestPointer(int voice, void *ptr) {
    (void)voice;
    for (int i=0;i<queue_count;++i) if (queue[i]==ptr) return 1;
    return 0;
}
int ASND_TestVoiceBufferReady(int voice) { (void)voice; return queue_count < 2; }
unsigned ASND_GetTimerVoice(int voice) { (void)voice; return 0; }

static void tick(void) {
    if (!paused && queue_count) {
        --queue_count;
        memmove(queue, queue+1, (size_t)queue_count*sizeof(*queue));
    }
    mp3_audio_pump();
}

static void scenario_reset(int type) {
    mode=type;
    failed_adds=failed_sets=queue_count=paused=0;
    sets=adds=stops=attempts=rejections=0;
}

static int full_play(const char *path, int type, int baseline_buffers) {
    scenario_reset(type);
    if (!mp3_audio_open(path)) {
        fprintf(stderr, "OPEN FAILED: %s\n",mp3_audio_error()); return -1;
    }
    int ticks=0;
    while (mp3_audio_active() && ticks<10000) { tick(); ++ticks; }
    int chunks=sets+adds;
    int good = !mp3_audio_active() && !mp3_audio_error()[0] &&
        chunks>10 && (baseline_buffers <= 0 || chunks == baseline_buffers);
    if (type==ONCE_BUSY || type==ONCE_INVALID || type==ONCE_SET_INVALID) good &= rejections==1;
    if (type==THREE_BUSY) good &= rejections==3;
    printf("%-23s %-4s ticks=%d queued=%d rejected=%d status='%s'\n",
        (const char *[]) {"normal replay", "busy once", "invalid once", "busy three times", "set-voice retry", "permanent busy"}[type],
        good ? "PASS" : "FAIL", ticks,chunks,rejections,mp3_audio_error());
    return good ? chunks : -1;
}

static int test_stop_pending(const char *path, int baseline_buffers) {
    scenario_reset(ONCE_BUSY);
    if (!mp3_audio_open(path)) return 0;
    for (int i=0; i<5 && mp3_audio_active(); ++i) tick();
    if (!rejections) { puts("stop with pending queue: FAIL (no rejection triggered)"); return 0; }
    mp3_audio_stop();
    if (mp3_audio_active()) return 0;
    int count=full_play(path,NORMAL,baseline_buffers);
    printf("stop / reopen after busy: %s\n",count>0 ? "PASS" : "FAIL");
    return count>0;
}

static int test_pause(const char *path) {
    scenario_reset(NORMAL);
    if (!mp3_audio_open(path)) return 0;
    for (int i=0;i<10;++i) tick();
    mp3_audio_pause_toggle();
    int before=queue_count;
    for (int i=0;i<10;++i) tick();
    int ok=mp3_audio_paused() && queue_count == before;
    mp3_audio_pause_toggle();
    ok &= !mp3_audio_paused();
    mp3_audio_stop();
    printf("pause / resume:          %s\n",ok ? "PASS" : "FAIL");
    return ok;
}

static int test_permanent_rejection(const char *path) {
    scenario_reset(ALWAYS_BUSY);
    if (!mp3_audio_open(path)) return 0;
    int ticks=0;
    while (mp3_audio_active() && ticks<10000) { tick(); ++ticks; }
    int ok = !mp3_audio_active() && rejections >= 300 &&
        strstr(mp3_audio_error(),"ASND queue failed repeatedly") != NULL;
    printf("persistent queue failure: %s rejected=%d error='%s'\n",
        ok ? "PASS" : "FAIL",rejections,mp3_audio_error());
    return ok;
}

int main(int argc, char **argv) {
    if (argc!=2) { fprintf(stderr,"Usage: %s '/path/to/test.mp3'\n",argv[0]); return 2; }
    if (!mp3_audio_init()) { fputs("audio init failed\n",stderr); return 3; }
    int ok=1;
    int baseline=full_play(argv[1],NORMAL,0);
    ok &= baseline>0;
    ok &= full_play(argv[1],ONCE_BUSY,baseline)>0;
    ok &= full_play(argv[1],ONCE_INVALID,baseline)>0;
    ok &= full_play(argv[1],THREE_BUSY,baseline)>0;
    ok &= full_play(argv[1],ONCE_SET_INVALID,baseline)>0;
    ok &= test_stop_pending(argv[1],baseline);
    ok &= test_pause(argv[1]);
    ok &= test_permanent_rejection(argv[1]);
    mp3_audio_shutdown();
    printf("REGRESSION RESULT: %s\n",ok?"PASS":"FAIL");
    return ok?0:1;
}
