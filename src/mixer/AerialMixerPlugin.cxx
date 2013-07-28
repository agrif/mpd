/*
 * Copyright (C) 2003-2013 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "MixerInternal.hxx"
#include "output/AerialOutputPlugin.hxx"
#include "conf.h"

#include <glib.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "aerial_mixer"

struct AerialMixer final : public Mixer {
	void *output;

	AerialMixer(void *_output)
		:Mixer(aerial_mixer_plugin),
		output(_output)
	{
	}
};

/**
 * The quark used for GError.domain.
 */
static inline GQuark
aerial_mixer_quark(void)
{
	return g_quark_from_static_string("aerial_mixer");
}

static Mixer *
aerial_mixer_init(void *ao, G_GNUC_UNUSED const struct config_param *param,
		 GError **error_r)
{
	if (ao == NULL) {
		g_set_error(error_r, aerial_mixer_quark(), 0,
			    "The aerial mixer cannot work without the audio output");
		return nullptr;
	}

	AerialMixer *am = new AerialMixer(ao);

	return am;
}

static void
aerial_mixer_finish(Mixer *data)
{
	AerialMixer *am = (AerialMixer *) data;

	delete am;
}

static int
aerial_mixer_get_volume(Mixer *mixer, G_GNUC_UNUSED GError **error_r)
{
	AerialMixer *am = (AerialMixer *) mixer;
	
	return aerial_output_get_volume((struct audio_output *)am->output);
}

static bool
aerial_mixer_set_volume(Mixer *mixer, unsigned volume, GError **error_r)
{
	AerialMixer *am = (AerialMixer *) mixer;
	
	aerial_output_set_volume((struct audio_output *)am->output, volume);
	return true;
}

const struct mixer_plugin aerial_mixer_plugin = {
	aerial_mixer_init,
	aerial_mixer_finish,
	nullptr,
	nullptr,
	aerial_mixer_get_volume,
	aerial_mixer_set_volume,
	false,
};
