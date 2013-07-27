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
	
	GError *async_error;
	AerialClient *client;
	
	gchar *queued_title;
	gchar *queued_artist;
	gchar *queued_album;
};

static inline GQuark
aerial_output_quark(void)
{
	return g_quark_from_static_string("aerial_output");
}

static struct audio_output *
aerial_output_init(const config_param *param, GError **error_r)
{
	g_type_init();
	
	AerialOutput *ad = new AerialOutput();
	if (!ao_base_init(&ad->base, &aerial_output_plugin, param, error_r)) {
		delete ad;
		return nullptr;
	}
	
	ad->host = config_get_block_string(param, "host", "localhost");
	ad->client = nullptr;
	ad->async_error = nullptr;
	
	ad->queued_title = nullptr;
	ad->queued_artist = nullptr;
	ad->queued_album = nullptr;
	
	return &ad->base;
}

static void
aerial_output_finish(struct audio_output *ao)
{
	AerialOutput *ad = (AerialOutput *)ao;
	if (ad->async_error)
		g_error_free(ad->async_error);
	ad->async_error = nullptr;
	
	ao_base_finish(&ad->base);
	delete ad;
}

static void
aerial_output_close(struct audio_output *ao)
{
	AerialOutput *ad = (AerialOutput *)ao;
	aerial_client_disconnect_from_host_async(ad->client, NULL, NULL);
	g_object_unref(ad->client);
	ad->client = nullptr;
	
	g_free(ad->queued_title);
	ad->queued_title = nullptr;
	g_free(ad->queued_artist);
	ad->queued_artist = nullptr;
	g_free(ad->queued_album);
	ad->queued_album = nullptr;
}

static void
aerial_on_error(GObject *src, GError *err, AerialOutput *ad)
{
	if (ad->async_error == nullptr) {
		ad->async_error = g_error_copy(err);
	}
}

static void
aerial_on_play(GObject *src, GAsyncResult *res, gpointer data)
{
	AerialClient *ac = AERIAL_CLIENT(src);
	AerialOutput *ad = (AerialOutput *)data;
	
	bool success = aerial_client_play_finish(ac, res, NULL);
	if (success) {
		if (ad->queued_title || ad->queued_artist || ad->queued_album)
			aerial_client_set_metadata_async(ac, ad->queued_title, ad->queued_artist, ad->queued_album, NULL, NULL, NULL);
		
		g_free(ad->queued_title);
		ad->queued_title = nullptr;
		g_free(ad->queued_artist);
		ad->queued_artist = nullptr;
		g_free(ad->queued_album);
		ad->queued_album = nullptr;
	}
}

static void
aerial_on_connect(GObject *src, GAsyncResult *res, gpointer data)
{
	AerialClient *ac = AERIAL_CLIENT(src);
	bool success = aerial_client_connect_to_host_finish(ac, res, NULL);
	if (success)
		aerial_client_play_async(ac, aerial_on_play, data);
}

static bool
aerial_output_open(struct audio_output *ao, struct audio_format *audio_format,
	       GError **error)
{
	AerialOutput *ad = (AerialOutput *)ao;

	if (ad->async_error) {
		g_propagate_error(error, ad->async_error);
		ad->async_error = nullptr;
		return false;
	}
	
	// our output format is fixed
	audio_format->format = SAMPLE_FORMAT_S16;
	audio_format->sample_rate = AERIAL_CLIENT_FRAMES_PER_SECOND;
	audio_format->channels = 2;

	ad->client = aerial_client_new();
	g_signal_connect(ad->client, "on-error", G_CALLBACK(aerial_on_error), ad);
	aerial_client_connect_to_host_async(ad->client, ad->host, aerial_on_connect, ao);

	return true;
}

static unsigned
aerial_output_delay(struct audio_output *ao)
{
	AerialOutput *ad = (AerialOutput *)ao;
	
	if (ad->async_error)
		return 0;
	
	if (aerial_client_get_state(ad->client) != AERIAL_CLIENT_STATE_PLAYING)
		return 100;
	if (aerial_client_get_write_space(ad->client) < AERIAL_CLIENT_BYTES_PER_FRAME)
		return aerial_client_get_local_buffer_length(ad->client) / 10;
	return 0;
}

static void
aerial_output_send_tag(struct audio_output *ao, const struct tag *tag)
{
	AerialOutput *ad = (AerialOutput *)ao;
	
	const char *title = tag_get_value(tag, TAG_TITLE);
	if (title == nullptr)
		title = tag_get_value(tag, TAG_NAME);
	const char *artist = tag_get_value(tag, TAG_ARTIST);
	if (artist == nullptr)
		title = tag_get_value(tag, TAG_ALBUM_ARTIST);
	const char *album = tag_get_value(tag, TAG_ALBUM);
	
	if (aerial_client_get_state(ad->client) != AERIAL_CLIENT_STATE_PLAYING) {
		g_free(ad->queued_title);
		ad->queued_title = g_strdup(title);
		g_free(ad->queued_artist);
		ad->queued_artist = g_strdup(artist);
		g_free(ad->queued_album);
		ad->queued_album = g_strdup(album);
	} else {
		aerial_client_set_metadata_async(ad->client, title, artist, album, NULL, NULL, NULL);
	}
}

static size_t
aerial_output_play(struct audio_output *ao, const void *chunk, size_t size,
				   GError **error)
{
	AerialOutput *ad = (AerialOutput *)ao;
	
	if (ad->async_error) {
		g_propagate_error(error, ad->async_error);
		ad->async_error = nullptr;
		return 0;
	}
	
	guint8 *buffer = reinterpret_cast<guint8*>(const_cast<void*>(chunk));
	size_t written = aerial_client_write(ad->client, buffer, size, NULL);
	
	if (written == 0)
		g_set_error(error, aerial_output_quark(), 0, "no more room");
	return written;
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
	aerial_output_delay,
	aerial_output_send_tag,
	aerial_output_play,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
};
