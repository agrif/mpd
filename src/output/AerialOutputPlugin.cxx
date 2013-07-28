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
#include "MixerList.hxx"

#include <glib.h>
#include <aerial.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "aerial"

struct AerialOutput {
	struct audio_output base;

	const char *host;
	
	GError *async_error;
	AerialClient *client;
	guint close_timeout;
	
	GStaticRecMutex coroutine_mutex;
	gboolean (* coroutine_finish)(AerialClient *, GAsyncResult *, GError **);
	bool running_coroutine;
	
	gchar *queued_title;
	gchar *queued_artist;
	gchar *queued_album;

	unsigned int volume;
	bool set_volume;
};

static inline GQuark
aerial_output_quark(void)
{
	return g_quark_from_static_string("aerial_output");
}

static void aerial_coroutine(AerialOutput *ad);

// used as a callback for *_async requests, to continue running the coroutine
static void
aerial_coroutine_async(GObject *src, GAsyncResult *res, gpointer data)
{
	AerialClient *ac = AERIAL_CLIENT(src);
	AerialOutput *ad = (AerialOutput *)data;
	
	g_static_rec_mutex_lock(&ad->coroutine_mutex);
	if (ad->coroutine_finish == nullptr) {
		// device must have been stopped before we got here
		g_static_rec_mutex_unlock(&ad->coroutine_mutex);
		return;
	}
	gboolean success = ad->coroutine_finish(ac, res, NULL);
	ad->coroutine_finish = nullptr;
	if (success) {
		aerial_coroutine(ad);
	}
	g_static_rec_mutex_unlock(&ad->coroutine_mutex);
}

// used to do *the next thing*, whatever that is. Controls requests to be sure
// that only one is active at once
static void
aerial_coroutine(AerialOutput *ad)
{
	g_static_rec_mutex_lock(&ad->coroutine_mutex);
	ad->running_coroutine = true;
	
	// if there's no client, skip everything
	if (ad->client == nullptr) {
		ad->running_coroutine = false;
		g_static_rec_mutex_unlock(&ad->coroutine_mutex);
		return;
	}
	
	AerialClientState state = aerial_client_get_state(ad->client);
	
	if (state == AERIAL_CLIENT_STATE_CONNECTED) {
		// if we've connected, transition to playing state
		ad->coroutine_finish = aerial_client_play_finish;
		aerial_client_play_async(ad->client, aerial_coroutine_async, ad);
	} else if (state >= AERIAL_CLIENT_STATE_READY &&
			   (ad->queued_title || ad->queued_artist || ad->queued_album)) {
		// there's metadata to set!
		ad->coroutine_finish = aerial_client_set_metadata_finish;
		aerial_client_set_metadata_async(ad->client, ad->queued_title, ad->queued_artist, ad->queued_album, NULL, aerial_coroutine_async, ad);
		
		g_free(ad->queued_title);
		g_free(ad->queued_artist);
		g_free(ad->queued_album);
		ad->queued_title = nullptr;
		ad->queued_artist = nullptr;
		ad->queued_album = nullptr;
	} else if (state >= AERIAL_CLIENT_STATE_READY && ad->set_volume) {
		// there's volume to set!
		ad->coroutine_finish = aerial_client_set_volume_finish;
		aerial_client_set_volume_async(ad->client, ad->volume / 100.0f, NULL, aerial_coroutine_async, ad);
		
		ad->set_volume = false;
	} else {
		// guess there's nothing to do
		ad->running_coroutine = false;
	}
	
	g_static_rec_mutex_unlock(&ad->coroutine_mutex);
}

// start the coroutine, if it's not started
static inline void
aerial_coroutine_start(AerialOutput *ad)
{
	g_static_rec_mutex_lock(&ad->coroutine_mutex);
	if (ad->running_coroutine == false)
		aerial_coroutine(ad);
	g_static_rec_mutex_unlock(&ad->coroutine_mutex);
}

static inline void
aerial_coroutine_set_metadata(AerialOutput *ad, const gchar *title, const gchar *artist, const gchar *album)
{
	g_static_rec_mutex_lock(&ad->coroutine_mutex);	
	g_free(ad->queued_title);
	ad->queued_title = g_strdup(title);
	g_free(ad->queued_artist);
	ad->queued_artist = g_strdup(artist);
	g_free(ad->queued_album);
	ad->queued_album = g_strdup(album);
	aerial_coroutine_start(ad);
	g_static_rec_mutex_unlock(&ad->coroutine_mutex);
}

static inline void
aerial_coroutine_set_volume(AerialOutput *ad, unsigned int volume)
{
	g_static_rec_mutex_lock(&ad->coroutine_mutex);	
	ad->volume = volume;
	ad->set_volume = true;
	aerial_coroutine_start(ad);
	g_static_rec_mutex_unlock(&ad->coroutine_mutex);	
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
	ad->close_timeout = 0;
	
	g_static_rec_mutex_init(&ad->coroutine_mutex);
	ad->coroutine_finish = nullptr;
	ad->running_coroutine = false;
	
	ad->queued_title = nullptr;
	ad->queued_artist = nullptr;
	ad->queued_album = nullptr;
	
	// TODO (??) read volume off device
	ad->volume = 100;
	ad->set_volume = false;
	
	return &ad->base;
}

static void
aerial_output_finish(struct audio_output *ao)
{
	AerialOutput *ad = (AerialOutput *)ao;
	if (ad->async_error)
		g_error_free(ad->async_error);
	ad->async_error = nullptr;
	
	g_static_rec_mutex_free(&ad->coroutine_mutex);
	
	ao_base_finish(&ad->base);
	delete ad;
}

static void aerial_output_close_real(AerialOutput *ad);

// sets our asynchronous error
static void
aerial_on_error(GObject *src, GError *err, AerialOutput *ad)
{
	if (ad->async_error == nullptr) {
		ad->async_error = g_error_copy(err);
	}
	aerial_output_close_real(ad);
}

static bool
aerial_output_open(struct audio_output *ao, struct audio_format *audio_format,
	       GError **error)
{
	AerialOutput *ad = (AerialOutput *)ao;

	g_static_rec_mutex_lock(&ad->coroutine_mutex);
	if (ad->async_error) {
		g_propagate_error(error, ad->async_error);
		ad->async_error = nullptr;
		g_static_rec_mutex_unlock(&ad->coroutine_mutex);
		return false;
	}
	
	// our output format is fixed
	audio_format->format = SAMPLE_FORMAT_S16;
	audio_format->sample_rate = AERIAL_CLIENT_FRAMES_PER_SECOND;
	audio_format->channels = 2;
	
	ad->close_timeout = 0; // tells the timeout *not* to run, if it exists

	// create a new client, if needed
	if (ad->client == nullptr) {
		ad->client = aerial_client_new();
		g_signal_connect(ad->client, "on-error", G_CALLBACK(aerial_on_error), ad);
		
		// start our coroutine off with connect_to_host
		ad->running_coroutine = false;
		ad->coroutine_finish = aerial_client_connect_to_host_finish;
		aerial_client_connect_to_host_async(ad->client, ad->host, aerial_coroutine_async, ad);
		aerial_coroutine_start(ad);
	
		// restore the volume
		aerial_coroutine_set_volume(ad, ad->volume);
	}
	g_static_rec_mutex_unlock(&ad->coroutine_mutex);

	return true;
}

static void
aerial_output_close_real(AerialOutput *ad)
{
	g_static_rec_mutex_lock(&ad->coroutine_mutex);
	if (ad->client) {
		aerial_client_disconnect_from_host_async(ad->client, NULL, NULL);
		g_object_unref(ad->client);
		ad->client = nullptr;
		ad->close_timeout = 0;
	}
	
	ad->coroutine_finish = nullptr;
	ad->running_coroutine = false;
	
	g_free(ad->queued_title);
	ad->queued_title = nullptr;
	g_free(ad->queued_artist);
	ad->queued_artist = nullptr;
	g_free(ad->queued_album);
	ad->queued_album = nullptr;
	g_static_rec_mutex_unlock(&ad->coroutine_mutex);
}

static bool
aerial_output_close_delayed(AerialOutput *ad)
{
	g_static_rec_mutex_lock(&ad->coroutine_mutex);
	if (ad->close_timeout > 0)
		aerial_output_close_real(ad);
	g_static_rec_mutex_unlock(&ad->coroutine_mutex);
	return false;
}

static void
aerial_output_close(struct audio_output *ao)
{
	AerialOutput *ad = (AerialOutput *)ao;
	// start a short timer to close this, if it hasn't been re-opened
	// to prevent the stream from dying when replacing the playlist
	ad->close_timeout =
		g_timeout_add(100, (GSourceFunc)aerial_output_close_delayed, ad);
}

static unsigned
aerial_output_delay(struct audio_output *ao)
{
	AerialOutput *ad = (AerialOutput *)ao;
	
	g_static_rec_mutex_lock(&ad->coroutine_mutex);
	if (ad->async_error) {
		g_static_rec_mutex_unlock(&ad->coroutine_mutex);
		return 0;
	}
	
	if (aerial_client_get_state(ad->client) != AERIAL_CLIENT_STATE_PLAYING) {
		g_static_rec_mutex_unlock(&ad->coroutine_mutex);
		return 100;
	}
	if (aerial_client_get_write_space(ad->client) < AERIAL_CLIENT_BYTES_PER_FRAME) {
		g_static_rec_mutex_unlock(&ad->coroutine_mutex);
		return aerial_client_get_local_buffer_length(ad->client) / 10;
	}
	g_static_rec_mutex_unlock(&ad->coroutine_mutex);
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
	
	aerial_coroutine_set_metadata(ad, title, artist, album);	
}

static size_t
aerial_output_play(struct audio_output *ao, const void *chunk, size_t size,
				   GError **error)
{
	AerialOutput *ad = (AerialOutput *)ao;
	
	g_static_rec_mutex_lock(&ad->coroutine_mutex);
	if (ad->async_error) {
		g_propagate_error(error, ad->async_error);
		ad->async_error = nullptr;
		g_static_rec_mutex_unlock(&ad->coroutine_mutex);
		return 0;
	}
	
	guint8 *buffer = reinterpret_cast<guint8*>(const_cast<void*>(chunk));
	size_t written = aerial_client_write(ad->client, buffer, size, NULL);
	g_static_rec_mutex_unlock(&ad->coroutine_mutex);
	
	if (written == 0)
		g_set_error(error, aerial_output_quark(), 0, "no more room");
	return written;
}

void
aerial_output_set_volume(struct audio_output *ao, unsigned volume)
{
	AerialOutput *ad = (AerialOutput *)ao;
	aerial_coroutine_set_volume(ad, volume);
}

unsigned
aerial_output_get_volume(struct audio_output *ao)
{
	AerialOutput *ad = (AerialOutput *)ao;
	return ad->volume;
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
	&aerial_mixer_plugin,
};
