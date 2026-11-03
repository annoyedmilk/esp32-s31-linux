// SPDX-License-Identifier: MIT
#ifndef MOSAICO_AUDIO_H
#define MOSAICO_AUDIO_H

#include <alsa/asoundlib.h>
#include <stdbool.h>
#include <stdint.h>

enum audio_phase { AUDIO_IDLE, AUDIO_PRIME, AUDIO_TONE, AUDIO_DRAIN, AUDIO_RECORD };

struct audio {
	snd_pcm_t *pcm;
	snd_ctl_t *ctl;
	enum audio_phase phase;
	bool armed, saved_controls;
	unsigned int frames, peak;
	long saved_volume, saved_output;
	uint64_t deadline, energy;
	char status[192];
};

void audio_init(struct audio *audio);
void audio_arm(struct audio *audio);
void audio_trigger(struct audio *audio, uint64_t now);
void audio_record(struct audio *audio, uint64_t now);
void audio_poll(struct audio *audio, uint64_t now);
void audio_close(struct audio *audio);

#endif
