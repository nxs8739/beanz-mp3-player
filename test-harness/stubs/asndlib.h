#ifndef ASNDLIB_H
#define ASNDLIB_H
#include <stdint.h>
#define VOICE_STEREO_16BIT 1
#define SND_OK 0
#define SND_INVALID -1
#define SND_BUSY 1
void ASND_Init(void); void ASND_Pause(int); void ASND_StopVoice(int); void ASND_PauseVoice(int,int); void ASND_End(void);
int ASND_SetVoice(int,int,int,int,void*,int,int,int,void*);
int ASND_AddVoice(int,void*,int);
int ASND_TestPointer(int,void*); int ASND_TestVoiceBufferReady(int); unsigned ASND_GetTimerVoice(int);
#endif
