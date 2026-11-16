// SPDX-License-Identifier: MIT
#include "audio.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define SAMPLE_RATE 16000U
#define TONE_FRAMES (SAMPLE_RATE / 2)
#define CHUNK_FRAMES 256U

static int mixer_value(snd_ctl_t *ctl, const char *name, long *number, bool write)
{
	snd_ctl_elem_info_t *info;
	snd_ctl_elem_value_t *value;
	int result;

	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_value_alloca(&value);
	snd_ctl_elem_info_set_interface(info, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_info_set_name(info, name);
	result = snd_ctl_elem_info(ctl, info);
	if (result < 0)
		return result;
	snd_ctl_elem_value_set_interface(value, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_value_set_name(value, name);
	result = snd_ctl_elem_read(ctl, value);
	if (result < 0)
		return result;
	if (write) {
		switch (snd_ctl_elem_info_get_type(info)) {
		case SND_CTL_ELEM_TYPE_BOOLEAN:
			snd_ctl_elem_value_set_boolean(value, 0, *number);
			break;
		case SND_CTL_ELEM_TYPE_INTEGER:
			if (*number < snd_ctl_elem_info_get_min(info) ||
			    *number > snd_ctl_elem_info_get_max(info))
				return -ERANGE;
			snd_ctl_elem_value_set_integer(value, 0, *number);
			break;
		case SND_CTL_ELEM_TYPE_ENUMERATED:
			if (*number < 0 || (unsigned long)*number >= snd_ctl_elem_info_get_items(info))
				return -ERANGE;
			snd_ctl_elem_value_set_enumerated(value, 0, *number);
			break;
		default:
			return -EINVAL;
		}
		return snd_ctl_elem_write(ctl, value);
	}
	if (snd_ctl_elem_info_get_type(info) == SND_CTL_ELEM_TYPE_ENUMERATED)
		*number = snd_ctl_elem_value_get_enumerated(value, 0);
	else
		*number = snd_ctl_elem_value_get_integer(value, 0);
	return 0;
}

static int mixer_set(snd_ctl_t *ctl, const char *name, long value)
{
	return mixer_value(ctl, name, &value, true);
}

static int select_headphones(snd_ctl_t *ctl)
{
	snd_ctl_elem_info_t *info;
	unsigned int items;
	int result;

	snd_ctl_elem_info_alloca(&info);
	snd_ctl_elem_info_set_interface(info, SND_CTL_ELEM_IFACE_MIXER);
	snd_ctl_elem_info_set_name(info, "Output Mode");
	result = snd_ctl_elem_info(ctl, info);
	if (result < 0)
		return result;
	items = snd_ctl_elem_info_get_items(info);
	for (unsigned int i = 0; i < items; i++) {
		snd_ctl_elem_info_set_item(info, i);
		result = snd_ctl_elem_info(ctl, info);
		if (result < 0)
			return result;
		if (!strcmp(snd_ctl_elem_info_get_item_name(info), "Headphones"))
			return mixer_set(ctl, "Output Mode", i);
	}
	return -ENOENT;
}

void audio_init(struct audio *audio)
{
	memset(audio, 0, sizeof(*audio));
	snprintf(audio->status, sizeof(audio->status),
		 "Arm playback, then press AI.\nNo sound is played on page entry.");
}

void audio_close(struct audio *audio)
{
	if (audio->ctl)
		mixer_set(audio->ctl, "Speaker Switch", 0);
	if (audio->pcm) {
		snd_pcm_drop(audio->pcm);
		snd_pcm_close(audio->pcm);
		audio->pcm = NULL;
	}
	if (audio->ctl) {
		if (audio->saved_controls) {
			mixer_set(audio->ctl, "DAC Playback Volume", audio->saved_volume);
			mixer_set(audio->ctl, "Output Mode", audio->saved_output);
		}
		snd_ctl_close(audio->ctl);
		audio->ctl = NULL;
	}
	audio->saved_controls = false;
	audio->armed = false;
	audio->phase = AUDIO_IDLE;
}

static void audio_failed(struct audio *audio, int error)
{
	snprintf(audio->status, sizeof(audio->status), "Audio stopped: %s", snd_strerror(error));
	audio_close(audio);
}

void audio_arm(struct audio *audio)
{
	if (audio->phase != AUDIO_IDLE)
		return;
	audio->armed = true;
	snprintf(audio->status, sizeof(audio->status),
		 "ARMED: press and release AI.\n440 Hz, 0.5 s, official BSP 50%%.\nPlayback is quiet on this board.");
}

static void audio_start(struct audio *audio, bool record, uint64_t now)
{
	snd_ctl_card_info_t *card;
	int result;

	if (audio->phase != AUDIO_IDLE)
		return;
	audio->armed = false;
	result = snd_ctl_open(&audio->ctl, "hw:0", 0);
	if (result < 0)
		goto fail;
	snd_ctl_card_info_alloca(&card);
	result = snd_ctl_card_info(audio->ctl, card);
	if (result < 0)
		goto fail;
	if (strcmp(snd_ctl_card_info_get_name(card), "ESP-Mosaico")) {
		result = -ENODEV;
		goto fail;
	}
	result = mixer_set(audio->ctl, "Speaker Switch", 0);
	if (result < 0)
		goto fail;
	if (!record) {
		result = mixer_value(audio->ctl, "DAC Playback Volume", &audio->saved_volume, false);
		if (result < 0)
			goto fail;
		result = mixer_value(audio->ctl, "Output Mode", &audio->saved_output, false);
		if (result < 0)
			goto fail;
		audio->saved_controls = true;
		/* Same ES8311 routing and 3.3 V volume compensation as the BSP. */
		result = mixer_set(audio->ctl, "DAC Playback Volume", 148);
		if (result < 0)
			goto fail;
		/* The codec's Headphones item enables the board's analogue output. */
		result = select_headphones(audio->ctl);
		if (result < 0)
			goto fail;
	}
	result = snd_pcm_open(&audio->pcm, "hw:0,0", record ? SND_PCM_STREAM_CAPTURE :
			      SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
	if (result < 0)
		goto fail;
	result = snd_pcm_set_params(audio->pcm, SND_PCM_FORMAT_S16_LE,
		SND_PCM_ACCESS_RW_INTERLEAVED, 2, SAMPLE_RATE, 0, 64000);
	if (result < 0)
		goto fail;
	audio->frames = audio->peak = 0;
	audio->energy = 0;
	audio->deadline = now + 4000;
	audio->phase = record ? AUDIO_RECORD : AUDIO_PRIME;
	snprintf(audio->status, sizeof(audio->status), record ?
		 "Recording 1 s; speaker disabled..." : "Priming with silence; speaker disabled...");
	if (record) {
		result = snd_pcm_start(audio->pcm);
		if (result < 0)
			goto fail;
	}
	return;
fail:
	audio_failed(audio, result);
}

void audio_trigger(struct audio *audio, uint64_t now)
{
	if (audio->armed)
		audio_start(audio, false, now);
}

void audio_record(struct audio *audio, uint64_t now)
{
	audio_start(audio, true, now);
}

static void fill_tone(int16_t *samples, unsigned int first, unsigned int count)
{
	for (unsigned int i = 0; i < count; i++) {
		unsigned int frame = first + i;
		double gain = 0.8;
		int16_t sample;

		if (frame < 80)
			gain *= frame / 80.0;
		if (frame >= TONE_FRAMES - 80)
			gain *= (TONE_FRAMES - frame - 1) / 80.0;
		sample = lrint(32767 * gain * sin(2 * 3.141592653589793 * 440 * frame / SAMPLE_RATE));
		samples[i * 2] = samples[i * 2 + 1] = sample;
	}
}

void audio_poll(struct audio *audio, uint64_t now)
{
	int16_t samples[CHUNK_FRAMES * 2];
	snd_pcm_sframes_t available, count;
	unsigned int remaining, frames;
	int result;

	if (audio->phase == AUDIO_IDLE)
		return;
	if (now >= audio->deadline) {
		audio_failed(audio, -ETIMEDOUT);
		return;
	}
	if (audio->phase == AUDIO_DRAIN) {
		result = snd_pcm_drain(audio->pcm);
		if (!result) {
			audio_close(audio);
			snprintf(audio->status, sizeof(audio->status), "Playback complete: 440 Hz / 0.5 s / 50%%.");
		} else if (result != -EAGAIN && result != -EINTR) {
			audio_failed(audio, result);
		}
		return;
	}
	/* Bound each GUI iteration even when the PCM ring has plenty of space. */
	for (unsigned int budget = 0; budget < 4; budget++) {
		available = snd_pcm_avail_update(audio->pcm);
		if (available == -EAGAIN || !available)
			return;
		if (available < 0) {
			audio_failed(audio, available);
			return;
		}
		remaining = (audio->phase == AUDIO_TONE ? TONE_FRAMES : SAMPLE_RATE) - audio->frames;
		frames = remaining < CHUNK_FRAMES ? remaining : CHUNK_FRAMES;
		if ((snd_pcm_sframes_t)frames > available)
			frames = available;
		if (audio->phase == AUDIO_RECORD) {
			count = snd_pcm_readi(audio->pcm, samples, frames);
		} else {
			memset(samples, 0, sizeof(samples));
			if (audio->phase == AUDIO_TONE)
				fill_tone(samples, audio->frames, frames);
			count = snd_pcm_writei(audio->pcm, samples, frames);
		}
		if (count == -EAGAIN || count == -EINTR || !count)
			return;
		if (count < 0) {
			audio_failed(audio, count);
			return;
		}
		if (audio->phase == AUDIO_RECORD) {
			for (snd_pcm_sframes_t i = 0; i < count; i++) {
				int sample = samples[i * 2];
				unsigned int magnitude = sample < 0 ? -sample : sample;

				audio->energy += (int64_t)sample * sample;
				if (magnitude > audio->peak)
					audio->peak = magnitude;
			}
		}
		audio->frames += count;
		if (audio->frames != (audio->phase == AUDIO_TONE ? TONE_FRAMES : SAMPLE_RATE))
			continue;
		if (audio->phase == AUDIO_RECORD) {
			snprintf(audio->status, sizeof(audio->status),
				 "Mic: 1 s recorded, not saved.\nRMS %.0f / 32768, peak %u / 32768.\nNo speaker playback.",
				 sqrt((double)audio->energy / audio->frames), audio->peak);
			audio_close(audio);
		} else if (audio->phase == AUDIO_PRIME) {
			result = mixer_set(audio->ctl, "Speaker Switch", 1);
			if (result < 0) {
				audio_failed(audio, result);
				return;
			}
			audio->phase = AUDIO_TONE;
			audio->frames = 0;
			snprintf(audio->status, sizeof(audio->status), "Playing 440 Hz for 0.5 s...");
		} else {
			audio->phase = AUDIO_DRAIN;
		}
		return;
	}
}
