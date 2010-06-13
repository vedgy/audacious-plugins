#include "settings.h"
#include "config.h"

#include <glib.h>
#include <audacious/i18n.h>

#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>

#include <audacious/plugin.h>
#include <audacious/ui_preferences.h>
#include <audacious/hook.h>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wchar.h>
#include <sys/time.h>

#include <curl/curl.h>

#include "plugin.h"
#include "scrobbler.h"
#include "gtkstuff.h"
#include "config.h"
#include "fmt.h"
#include "configure.h"

typedef struct submit_t
{
	int dosubmit, pos_c, len;
} submit_t;

static void init(void);
static void cleanup(void);
static int sc_going, ge_going;

static GMutex *m_scrobbler;

guint track_timeout = 0;

Tuple *submit_tuple = NULL;

extern PluginPreferences preferences;

static GeneralPlugin scrobbler_gp =
{
	.description = "Scrobbler Plugin",
	.init = init,
	.about = about_show,
	.cleanup = cleanup,
	.settings = &preferences,
};

static gboolean ishttp(const char *a)
{
	g_return_val_if_fail(a != NULL, FALSE);
	return aud_str_has_prefix_nocase(a, "http://") || aud_str_has_prefix_nocase(a, "https://");
}

static void aud_hook_playback_begin(gpointer hook_data, gpointer user_data)
{
	gint playlist = aud_playlist_get_active();
	gint pos = aud_playlist_get_position(playlist);
	const Tuple *tuple;

	if (aud_playlist_entry_get_length(playlist, pos) < (glong)30)
	{
		AUDDBG(" *** not submitting due to entry->length < 30");
		return;
	}

	if (ishttp(aud_playlist_entry_get_filename(playlist, pos)))
	{
		AUDDBG(" *** not submitting due to HTTP source");
		return;
	}

	tuple = aud_playlist_entry_get_tuple(playlist, pos);
	if (tuple == NULL)
		return;

	submit_tuple = tuple_copy(tuple);
	sc_addentry(m_scrobbler, submit_tuple, aud_tuple_get_int(submit_tuple, FIELD_LENGTH, NULL) / 1000);

	if (!track_timeout)
		track_timeout = g_timeout_add_seconds(1, sc_timeout, NULL);
}

static void aud_hook_playback_end(gpointer aud_hook_data, gpointer user_data)
{
	sc_idle(m_scrobbler);

	if (track_timeout)
	{
		g_source_remove(track_timeout);
		track_timeout = 0;
	}

	if (submit_tuple != NULL)
	{
		mowgli_object_unref(submit_tuple);
		submit_tuple = NULL;
	}
}

void start(void) {
	char *username = NULL, *password = NULL, *sc_url = NULL;
	char *ge_username = NULL, *ge_password = NULL;
	mcs_handle_t *cfgfile;
	sc_going = 1;
	ge_going = 1;

	if ((cfgfile = aud_cfg_db_open()) != NULL) {
		aud_cfg_db_get_string(cfgfile, "audioscrobbler", "username",
				&username);
		aud_cfg_db_get_string(cfgfile, "audioscrobbler", "password",
				&password);
		aud_cfg_db_get_string(cfgfile, "audioscrobbler", "sc_url",
				&sc_url);		
		aud_cfg_db_get_string(cfgfile, "audioscrobbler", "ge_username",
				&ge_username);
		aud_cfg_db_get_string(cfgfile, "audioscrobbler", "ge_password",
				&ge_password);
		aud_cfg_db_close(cfgfile);
	}

	if ((!username || !password) || (!*username || !*password))
	{
		AUDDBG("username/password not found - not starting last.fm support",
			DEBUG);
		sc_going = 0;
	}
	else
	{
		sc_init(username, password, sc_url);

		g_free(username);
		g_free(password);
		g_free(sc_url);
	}
	
	m_scrobbler = g_mutex_new();

	aud_hook_associate("playback begin", aud_hook_playback_begin, NULL);
	aud_hook_associate("playback stop", aud_hook_playback_end, NULL);

	AUDDBG("plugin started");
	sc_idle(m_scrobbler);
}

void stop(void) {
	if (!sc_going && !ge_going)
		return;
	g_mutex_lock(m_scrobbler);

	if (sc_going)
		sc_cleaner();
	sc_going = 0;
	ge_going = 0;
	g_mutex_unlock(m_scrobbler);

	g_mutex_free(m_scrobbler);

	aud_hook_dissociate("playback begin", aud_hook_playback_begin);
	aud_hook_dissociate("playback stop", aud_hook_playback_end);
}

static void init(void)
{
    start();
}

static void cleanup(void)
{
    stop();
}

void setup_proxy(CURL *curl)
{
    mcs_handle_t *db;
    gboolean use_proxy = FALSE;

    db = aud_cfg_db_open();
    aud_cfg_db_get_bool(db, NULL, "use_proxy", &use_proxy);
    if (use_proxy == FALSE)
    {
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
    }
    else
    {
        gchar *proxy_host = NULL, *proxy_port = NULL;
        gboolean proxy_use_auth = FALSE;
        aud_cfg_db_get_string(db, NULL, "proxy_host", &proxy_host);
        aud_cfg_db_get_string(db, NULL, "proxy_port", &proxy_port);
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_host);
        curl_easy_setopt(curl, CURLOPT_PROXYPORT, atol(proxy_port));
        aud_cfg_db_get_bool(db, NULL, "proxy_use_auth", &proxy_use_auth);
        if (proxy_use_auth != FALSE)
        {
            gchar *userpwd = NULL, *user = NULL, *pass = NULL;
            aud_cfg_db_get_string(db, NULL, "proxy_user", &user);
            aud_cfg_db_get_string(db, NULL, "proxy_pass", &pass);
            userpwd = g_strdup_printf("%s:%s", user, pass);
            curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, userpwd);
            g_free(userpwd);
        }
    }
    aud_cfg_db_close(db);
}

GeneralPlugin *scrobbler_gplist[] = { &scrobbler_gp, NULL };

DECLARE_PLUGIN(scrobbler, NULL, NULL, NULL, NULL, NULL, scrobbler_gplist, NULL, NULL);
