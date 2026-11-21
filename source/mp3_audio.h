#ifndef BEANZ_MP3_AUDIO_H
#define BEANZ_MP3_AUDIO_H
int mp3_audio_init(void);
int mp3_audio_open(const char *path);
void mp3_audio_pump(void);
void mp3_audio_pause_toggle(void);
void mp3_audio_stop(void);
void mp3_audio_shutdown(void);
int mp3_audio_active(void);
int mp3_audio_paused(void);
unsigned mp3_audio_elapsed_seconds(void);
unsigned mp3_audio_total_seconds(void);
const char *mp3_audio_error(void);
#endif
