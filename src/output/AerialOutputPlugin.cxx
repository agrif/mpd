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
#include "AerialOutputPlugin.hxx"
#include "output_api.h"

#include <glib.h>
#include <aerial.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "aerial"

struct AerialOutput {
	struct audio_output base;

	const char *host;
	
	AerialClient *client;
};

static inline GQuark
aerial_output_quark(void)
{
	return g_quark_from_static_string("aerial_output");
}

static struct audio_output *
aerial_output_init(const config_param *param, GError **error_r)
{
	AerialOutput *ad = new AerialOutput();
	if (!ao_base_init(&ad->base, &aerial_output_plugin, param, error_r)) {
		delete ad;
		return nullptr;
	}
	
	ad->host = config_get_block_string(param, "host", "localhost");
	ad->client = nullptr;
	
	return &ad->base;
}

static void
aerial_output_finish(struct audio_output *ao)
{
	AerialOutput *ad = (AerialOutput *)ao;
	
	ao_base_finish(&ad->base);
	delete ad;
}

static void
aerial_output_close(struct audio_output *ao)
{
	AerialOutput *ad = (AerialOutput *)ao;
	aerial_client_disconnect_from_host(ad->client, NULL);
}

static bool
aerial_output_open(struct audio_output *ao, struct audio_format *audio_format,
	       GError **error)
{
	AerialOutput *ad = (AerialOutput *)ao;
	
	// our output format is fixed
	audio_format->format = SAMPLE_FORMAT_S16;
	audio_format->sample_rate = AERIAL_CLIENT_FRAMES_PER_SECOND;
	audio_format->channels = 2;

	ad->client = aerial_client_new();
	if (!aerial_client_connect_to_host(ad->client, ad->host, error))
		return false;
	if (!aerial_client_play(ad->client, error))
		return false;

	return true;
}

static size_t
aerial_output_play(struct audio_output *ao, const void *chunk, size_t size,
				   GError **error)
{
	AerialOutput *ad = (AerialOutput *)ao;

	if (aerial_client_get_state(ad->client) != AERIAL_CLIENT_STATE_PLAYING) {
		if (!aerial_client_play(ad->client, error))
			return 0;
	}
	
	guint8 *buffer = reinterpret_cast<guint8*>(const_cast<void*>(chunk));
	return aerial_client_write(ad->client, buffer, size, NULL);
}

const struct audio_output_plugin aerial_output_plugin = {
	"aerial",
	nullptr,
	aerial_output_init,
	aerial_output_finish,
	nullptr,
	nullptr,
	aerial_output_open,
	aerial_output_close,
	nullptr,
	nullptr,
	aerial_output_play,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
};
