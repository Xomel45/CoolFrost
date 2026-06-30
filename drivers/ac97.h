#ifndef AC97_H
#define AC97_H

#include <stdint.h>

/* Detect Intel AC97 (82801AA/AB/BA/CA), init at 48000 Hz 16-bit stereo.
 * Returns 0 if device found and initialised, -1 otherwise. */
int  ac97_init(void);

/* Play n_frames stereo frames (left/right interleaved int16_t pairs).
 * Blocks until playback completes (polling). */
void ac97_play_pcm(const int16_t *frames, uint32_t n_frames);

/* Generate and play a square wave: freq Hz for ms milliseconds. */
void ac97_beep(uint32_t freq, uint32_t ms);

#endif
