#ifndef AUDIO_H
#define AUDIO_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize MAX98357A I2S audio.
 *
 * MAX98357A:
 *
 * BCLK -> GPIO45
 * LRC  -> GPIO46
 * DIN  -> GPIO47
 */
esp_err_t audio_init(void);

/*
 * Initialize offline PicoTTS.
 */
esp_err_t audio_tts_init(void);

/*
 * Speak dynamic text.
 *
 * Example:
 *
 * audio_speak(
 *     "Good morning Anup Kumar. Have a nice day."
 * );
 */
esp_err_t audio_speak(const char *text);

/*
 * Existing WAV test.
 *
 * File:
 * /spiflash/audio/good_morning.wav
 */
esp_err_t audio_play_test(void);

/*
 * Returns true while TTS is speaking.
 */
bool audio_is_busy(void);

#ifdef __cplusplus
}
#endif

#endif