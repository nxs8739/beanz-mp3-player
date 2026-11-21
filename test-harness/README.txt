Host-side ASND queue retry regression harness
=============================================

Compiled on host Linux with gcc, does not require devkitPPC.
From the project top directory, run:

  gcc -std=gnu11 -O1 -Wall -Wextra -Itest-harness/stubs -Isource \
      test-harness/test_replay.c source/mp3_audio.c -lm -o test-harness/run
  test-harness/run '/path/to/your/own/test.mp3'

The runner uses a simulated ASND audio queue. Test cases: normal full playback,
transient BUSY, transient INVALID, three consecutive BUSY returns, a
SetVoice startup rejection, stop/reopen after rejection, pause/resume,
and a persistent queue failure. Successful full tracks must queue the
same number of PCM chunks as the normal baseline.

No copyrighted MP3 is embedded or distributed. The user's private MP3
is only used by passing its path at test runtime.

Mocked ASND tests C logic only; they do not replace real GameCube testing.
