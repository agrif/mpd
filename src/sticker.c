/*
 * Copyright (C) 2003-2009 The Music Player Daemon Project
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sticker.h"

#include <glib.h>
#include <sqlite3.h>
#include <assert.h>

#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN "sticker"

static const char sticker_sql_create[] =
	"CREATE TABLE IF NOT EXISTS sticker("
	"  type VARCHAR NOT NULL, "
	"  uri VARCHAR NOT NULL, "
	"  name VARCHAR NOT NULL, "
	"  value VARCHAR NOT NULL"
	");"
	"CREATE UNIQUE INDEX IF NOT EXISTS"
	" sticker_value ON sticker(type, uri, name);"
	"";

static const char sticker_sql_get[] =
	"SELECT value FROM sticker WHERE type=? AND uri=? AND name=?";

static const char sticker_sql_update[] =
	"UPDATE sticker SET value=? WHERE type=? AND uri=? AND name=?";

static const char sticker_sql_insert[] =
	"INSERT INTO sticker(type,uri,name,value) VALUES(?, ?, ?, ?)";

static const char sticker_sql_delete[] =
	"DELETE FROM sticker WHERE type=? AND uri=?";

static sqlite3 *sticker_db;
static sqlite3_stmt *sticker_stmt_get, *sticker_stmt_update,
	*sticker_stmt_insert, *sticker_stmt_delete;

static sqlite3_stmt *
sticker_prepare(const char *sql)
{
	int ret;
	sqlite3_stmt *stmt;

	ret = sqlite3_prepare_v2(sticker_db, sql, -1, &stmt, NULL);
	if (ret != SQLITE_OK)
		g_error("sqlite3_prepare_v2() failed: %s",
			sqlite3_errmsg(sticker_db));

	return stmt;
}

void
sticker_global_init(const char *path)
{
	int ret;

	if (path == NULL)
		/* not configured */
		return;

	/* open/create the sqlite database */

	ret = sqlite3_open(path, &sticker_db);
	if (ret != SQLITE_OK)
		g_error("Failed to open sqlite database '%s': %s",
			path, sqlite3_errmsg(sticker_db));

	/* create the table and index */

	ret = sqlite3_exec(sticker_db, sticker_sql_create, NULL, NULL, NULL);
	if (ret != SQLITE_OK)
		g_error("Failed to create sticker table: %s",
			sqlite3_errmsg(sticker_db));

	/* prepare the statements we're going to use */

	sticker_stmt_get = sticker_prepare(sticker_sql_get);
	sticker_stmt_update = sticker_prepare(sticker_sql_update);
	sticker_stmt_insert = sticker_prepare(sticker_sql_insert);
	sticker_stmt_delete = sticker_prepare(sticker_sql_delete);

	if (sticker_stmt_get == NULL || sticker_stmt_update == NULL ||
	    sticker_stmt_insert == NULL || sticker_stmt_delete == NULL)
		g_error("Failed to prepare sqlite statements");
}

void
sticker_global_finish(void)
{
	if (sticker_db == NULL)
		/* not configured */
		return;

	sqlite3_finalize(sticker_stmt_delete);
	sqlite3_finalize(sticker_stmt_update);
	sqlite3_finalize(sticker_stmt_insert);
	sqlite3_close(sticker_db);
}

bool
sticker_enabled(void)
{
	return sticker_db != NULL;
}

char *
sticker_load_value(const char *type, const char *uri, const char *name)
{
	int ret;
	char *value;

	assert(sticker_enabled());
	assert(type != NULL);
	assert(uri != NULL);
	assert(name != NULL);

	if (*name == 0)
		return NULL;

	sqlite3_reset(sticker_stmt_get);

	ret = sqlite3_bind_text(sticker_stmt_get, 1, type, -1, NULL);
	if (ret != SQLITE_OK) {
		g_warning("sqlite3_bind_text() failed: %s",
			  sqlite3_errmsg(sticker_db));
		return false;
	}

	ret = sqlite3_bind_text(sticker_stmt_get, 2, uri, -1, NULL);
	if (ret != SQLITE_OK) {
		g_warning("sqlite3_bind_text() failed: %s",
			  sqlite3_errmsg(sticker_db));
		return false;
	}

	ret = sqlite3_bind_text(sticker_stmt_get, 3, name, -1, NULL);
	if (ret != SQLITE_OK) {
		g_warning("sqlite3_bind_text() failed: %s",
			  sqlite3_errmsg(sticker_db));
		return false;
	}

	do {
		ret = sqlite3_step(sticker_stmt_get);
	} while (ret == SQLITE_BUSY);

	if (ret == SQLITE_ROW) {
		/* record found */
		value = g_strdup((const char*)sqlite3_column_text(sticker_stmt_get, 0));
	} else if (ret == SQLITE_DONE) {
		/* no record found */
		value = NULL;
	} else {
		/* error */
		g_warning("sqlite3_step() failed: %s",
			  sqlite3_errmsg(sticker_db));
		return false;
	}

	sqlite3_reset(sticker_stmt_get);
	sqlite3_clear_bindings(sticker_stmt_get);

	return value;
}

static bool
sticker_update_value(const char *type, const char *uri,
		     const char *name, const char *value)
{
	int ret;

	assert(type != NULL);
	assert(uri != NULL);
	assert(name != NULL);
	assert(*name != 0);
	assert(value != NULL);

	assert(sticker_enabled());

	sqlite3_reset(sticker_stmt_update);

	ret = sqlite3_bind_text(sticker_stmt_update, 1, value, -1, NULL);
	if (ret != SQLITE_OK) {
		g_warning("sqlite3_bind_text() failed: %s",
			  sqlite3_errmsg(sticker_db));
		return false;
	}

	ret = sqlite3_bind_text(sticker_stmt_update, 2, type, -1, NULL);
	if (ret != SQLITE_OK) {
		g_warning("sqlite3_bind_text() failed: %s",
			  sqlite3_errmsg(sticker_db));
		return false;
	}

	ret = sqlite3_bind_text(sticker_stmt_update, 3, uri, -1, NULL);
	if (ret != SQLITE_OK) {
		g_warning("sqlite3_bind_text() failed: %s",
			  sqlite3_errmsg(sticker_db));
		return false;
	}

	ret = sqlite3_bind_text(sticker_stmt_update, 4, name, -1, NULL);
	if (ret != SQLITE_OK) {
		g_warning("sqlite3_bind_text() failed: %s",
			  sqlite3_errmsg(sticker_db));
		return false;
	}

	do {
		ret = sqlite3_step(sticker_stmt_update);
	} while (ret == SQLITE_BUSY);

	if (ret != SQLITE_DONE) {
		g_warning("sqlite3_step() failed: %s",
			  sqlite3_errmsg(sticker_db));
		return false;
	}

	ret = sqlite3_changes(sticker_db);

	sqlite3_reset(sticker_stmt_update);
	sqlite3_clear_bindings(sticker_stmt_update);

	return ret > 0;
}

static bool
sticker_insert_value(const char *type, const char *uri,
		     const char *name, const char *value)
{
	int ret;

	assert(type != NULL);
	assert(uri != NULL);
	assert(name != NULL);
	assert(*name != 0);
	assert(value != NULL);

	assert(sticker_enabled());

	sqlite3_reset(sticker_stmt_insert);

	ret = sqlite3_bind_text(sticker_stmt_insert, 1, type, -1, NULL);
	if (ret != SQLITE_OK) {
		g_warning("sqlite3_bind_text() failed: %s",
			  sqlite3_errmsg(sticker_db));
		return false;
	}

	ret = sqlite3_bind_text(sticker_stmt_insert, 2, uri, -1, NULL);
	if (ret != SQLITE_OK) {
		g_warning("sqlite3_bind_text() failed: %s",
			  sqlite3_errmsg(sticker_db));
		return false;
	}

	ret = sqlite3_bind_text(sticker_stmt_insert, 3, name, -1, NULL);
	if (ret != SQLITE_OK) {
		g_warning("sqlite3_bind_text() failed: %s",
			  sqlite3_errmsg(sticker_db));
		return false;
	}

	ret = sqlite3_bind_text(sticker_stmt_insert, 4, value, -1, NULL);
	if (ret != SQLITE_OK) {
		g_warning("sqlite3_bind_text() failed: %s",
			  sqlite3_errmsg(sticker_db));
		return false;
	}

	do {
		ret = sqlite3_step(sticker_stmt_insert);
	} while (ret == SQLITE_BUSY);

	if (ret != SQLITE_DONE) {
		g_warning("sqlite3_step() failed: %s",
			  sqlite3_errmsg(sticker_db));
		return false;
	}

	sqlite3_reset(sticker_stmt_insert);
	sqlite3_clear_bindings(sticker_stmt_insert);

	return true;
}

bool
sticker_store_value(const char *type, const char *uri,
		    const char *name, const char *value)
{
	assert(sticker_enabled());
	assert(type != NULL);
	assert(uri != NULL);
	assert(name != NULL);
	assert(value != NULL);

	if (*name == 0)
		return false;

	return sticker_update_value(type, uri, name, value) ||
		sticker_insert_value(type, uri, name, value);
}

bool
sticker_delete(const char *type, const char *uri)
{
	int ret;

	assert(sticker_enabled());
	assert(type != NULL);
	assert(uri != NULL);

	sqlite3_reset(sticker_stmt_delete);

	ret = sqlite3_bind_text(sticker_stmt_delete, 1, type, -1, NULL);
	if (ret != SQLITE_OK) {
		g_warning("sqlite3_bind_text() failed: %s",
			  sqlite3_errmsg(sticker_db));
		return false;
	}

	ret = sqlite3_bind_text(sticker_stmt_delete, 2, uri, -1, NULL);
	if (ret != SQLITE_OK) {
		g_warning("sqlite3_bind_text() failed: %s",
			  sqlite3_errmsg(sticker_db));
		return false;
	}

	do {
		ret = sqlite3_step(sticker_stmt_delete);
	} while (ret == SQLITE_BUSY);

	if (ret != SQLITE_DONE) {
		g_warning("sqlite3_step() failed: %s",
			  sqlite3_errmsg(sticker_db));
		return false;
	}

	sqlite3_reset(sticker_stmt_delete);
	sqlite3_clear_bindings(sticker_stmt_delete);

	return true;
}