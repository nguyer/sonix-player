// Included by library.c to share its database lock, collation and generation
// counters. All SQL statements here are private; scan statements are untouched.

static bool remote_names(const char *table, const char *column, const char *expression) {
	char select_sql[256], insert_sql[256];
	snprintf(select_sql, sizeof(select_sql), "SELECT DISTINCT %s FROM REMOTE_MEDIA", expression);
	snprintf(insert_sql, sizeof(insert_sql), "INSERT OR IGNORE INTO %s(id,%s,character,cn,sortkey) VALUES(0,?,?,?,?)", table, column);
	sqlite3_stmt *read = NULL, *write = NULL;
	bool ok = sqlite3_prepare_v2(db, select_sql, -1, &read, NULL) == SQLITE_OK &&
		sqlite3_prepare_v2(db, insert_sql, -1, &write, NULL) == SQLITE_OK;
	while (ok && sqlite3_step(read) == SQLITE_ROW) {
		const char *value = (const char *)sqlite3_column_text(read, 0);
		if (value && *value) run_lookup(write, value);
	}
	sqlite3_finalize(read); sqlite3_finalize(write);
	return ok;
}

static bool remote_restore(void) {
	bool ok = exec("INSERT INTO MEDIA_TABLE(id,path,name,album,artist,album_artist,genre,year,dis_id,disc,"
		"character,sortkey,album_key,size,mtime,ctime,cue_id) "
		"SELECT -r.rowid,r.path,r.title,r.album,r.artist,COALESCE(NULLIF(r.album_artist,''),r.artist),r.genre,r.year,"
		"r.track,MAX(1,r.disc),r.character,r.sortkey,r.album_key,"
		"0,0,0,0 FROM REMOTE_MEDIA r WHERE NOT EXISTS(SELECT 1 FROM MEDIA_TABLE m WHERE m.path=r.path)");
	// Older builds let the local artist/folder organizer rewrite remote keys.
	// Repair those rows before building groups, retaining existing media rowids.
	ok = exec("UPDATE MEDIA_TABLE SET album_key=(SELECT r.album_key FROM REMOTE_MEDIA r WHERE r.path=MEDIA_TABLE.path) "
		"WHERE EXISTS(SELECT 1 FROM REMOTE_MEDIA r WHERE r.path=MEDIA_TABLE.path "
		"AND (MEDIA_TABLE.album_key IS NULL OR MEDIA_TABLE.album_key<>r.album_key))") && ok;
	ok = remote_names("ALBUM_TABLE", "album", "album") && ok;
	ok = exec("INSERT OR IGNORE INTO ALBUM_GROUP_TABLE(album,album_key,sortkey) "
		"SELECT m.album,m.album_key,sortkey(m.album) FROM MEDIA_TABLE m "
		"WHERE m.album<>'' AND EXISTS(SELECT 1 FROM REMOTE_MEDIA r WHERE r.path=m.path)") && ok;
	ok = exec("DELETE FROM ALBUM_GROUP_TABLE WHERE album IN (SELECT album FROM REMOTE_MEDIA) "
		"AND NOT EXISTS(SELECT 1 FROM MEDIA_TABLE m WHERE m.album=ALBUM_GROUP_TABLE.album "
		"AND m.album_key=ALBUM_GROUP_TABLE.album_key)") && ok;
	ok = remote_names("ARTIST_TABLE", "artist", "artist") && ok;
	ok = remote_names("GENRE_TABLE", "genre", "genre") && ok;
	ok = remote_names("ALBUM_ARTIST_TABLE", "album_artist", "COALESCE(NULLIF(album_artist,''),artist)") && ok;
	ok = exec("UPDATE ALBUM_GROUP_TABLE SET sortkey=(SELECT a.sortkey FROM ALBUM_TABLE a WHERE a.album=ALBUM_GROUP_TABLE.album) "
		"WHERE album IN (SELECT album FROM REMOTE_MEDIA)") && ok;
	ok = exec("DELETE FROM COUNT_TABLE") && ok;
	ok = exec("INSERT INTO COUNT_TABLE(cn) SELECT COUNT(*) FROM MEDIA_TABLE") && ok;
	return ok;
}

bool library_remote_begin(const char *source) {
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *s = NULL;
	bool ok = db && !scan_running && source && *source &&
		sqlite3_prepare_v2(db, "DELETE FROM REMOTE_STAGE WHERE source=?", -1, &s, NULL) == SQLITE_OK;
	if (ok) { sqlite3_bind_text(s, 1, source, -1, SQLITE_TRANSIENT); ok = sqlite3_step(s) == SQLITE_DONE; }
	sqlite3_finalize(s);
	pthread_mutex_unlock(&db_lock);
	return ok;
}

bool library_remote_stage(const library_remote_track_t *rows, int count) {
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *s = NULL;
	bool transaction = db && !scan_running && rows && count >= 0 && exec("BEGIN");
	bool ok = transaction;
	if (ok) ok = sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO REMOTE_STAGE VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
		-1, &s, NULL) == SQLITE_OK;
	for (int i = 0; ok && i < count; ++i) {
		const library_remote_track_t *r = &rows[i];
		char sort[LIBRARY_SORT_KEY_MAX], character[16], akey[256];
		int group;
		library_sort_key(r->song.title, sort, sizeof(sort));
		index_character(r->song.title, character, sizeof(character), &group);
		snprintf(akey, sizeof(akey), "remote:%s:%s", r->source, r->song.album_id[0] ? r->song.album_id : r->song.album);
		const char *values[] = {r->path,r->source,r->server,r->username,r->song.id,r->song.album_id,
			r->song.title,r->song.artist,r->song.album,r->song.cover_id};
		for (int k = 0; k < 10; ++k) sqlite3_bind_text(s, k + 1, values[k], -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(s, 11, r->song.duration); sqlite3_bind_int(s, 12, r->song.track);
		sqlite3_bind_text(s, 13, r->song.suffix, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(s, 14, sort, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(s, 15, character, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(s, 16, akey, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(s, 17, r->song.album_artist, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(s, 18, r->song.genre, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(s, 19, r->song.year); sqlite3_bind_int(s, 20, r->song.disc);
		ok = sqlite3_step(s) == SQLITE_DONE;
		sqlite3_reset(s); sqlite3_clear_bindings(s);
	}
	sqlite3_finalize(s);
	if (ok) ok = exec("COMMIT");
	if (!ok && transaction) exec("ROLLBACK");
	pthread_mutex_unlock(&db_lock);
	return ok;
}

bool library_remote_publish(const char *source) {
	pthread_mutex_lock(&db_lock);
	bool transaction = db && !scan_running && source && *source && exec("BEGIN");
	bool ok = transaction;
	const char *sql[] = {
		"DELETE FROM MEDIA_TABLE WHERE path IN (SELECT path FROM REMOTE_MEDIA WHERE source=?)",
		"DELETE FROM REMOTE_MEDIA WHERE source=?",
		"INSERT INTO REMOTE_MEDIA SELECT * FROM REMOTE_STAGE WHERE source=?",
		"DELETE FROM REMOTE_STAGE WHERE source=?"};
	for (int i = 0; ok && i < 4; ++i) {
		sqlite3_stmt *s = NULL;
		ok = sqlite3_prepare_v2(db, sql[i], -1, &s, NULL) == SQLITE_OK;
		if (ok) { sqlite3_bind_text(s, 1, source, -1, SQLITE_TRANSIENT); ok = sqlite3_step(s) == SQLITE_DONE; }
		sqlite3_finalize(s);
	}
	if (ok) ok = remote_restore();
	if (ok) { prune_links(); prune_names(); ok = exec("COMMIT"); }
	if (!ok && transaction) exec("ROLLBACK");
	if (ok) bump_generation(GEN_MEDIA);
	pthread_mutex_unlock(&db_lock);
	return ok;
}

bool library_remote_get(const char *path, library_remote_track_t *r) {
	memset(r, 0, sizeof(*r));
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *s = NULL;
	bool ok = db && path && sqlite3_prepare_v2(db, "SELECT * FROM REMOTE_MEDIA WHERE path=?", -1, &s, NULL) == SQLITE_OK;
	if (ok) { sqlite3_bind_text(s, 1, path, -1, SQLITE_TRANSIENT); ok = sqlite3_step(s) == SQLITE_ROW; }
	if (ok) {
		char *fields[] = {r->path,r->source,r->server,r->username,r->song.id,r->song.album_id,
			r->song.title,r->song.artist,r->song.album,r->song.cover_id};
		const size_t sizes[] = {sizeof(r->path),sizeof(r->source),sizeof(r->server),sizeof(r->username),sizeof(r->song.id),
			sizeof(r->song.album_id),sizeof(r->song.title),sizeof(r->song.artist),sizeof(r->song.album),sizeof(r->song.cover_id)};
		for (int i = 0; i < 10; ++i) {
			const char *value = (const char *)sqlite3_column_text(s, i);
			snprintf(fields[i], sizes[i], "%s", value ? value : "");
		}
		r->song.duration = sqlite3_column_int(s, 10); r->song.track = sqlite3_column_int(s, 11);
		const char *suffix = (const char *)sqlite3_column_text(s, 12);
		snprintf(r->song.suffix, sizeof(r->song.suffix), "%s", suffix ? suffix : "");
		const char *aa = (const char *)sqlite3_column_text(s, 16);
		const char *genre = (const char *)sqlite3_column_text(s, 17);
		snprintf(r->song.album_artist, sizeof(r->song.album_artist), "%s", aa ? aa : "");
		snprintf(r->song.genre, sizeof(r->song.genre), "%s", genre ? genre : "");
		r->song.year = sqlite3_column_int(s, 18); r->song.disc = sqlite3_column_int(s, 19);
	}
	sqlite3_finalize(s);
	pthread_mutex_unlock(&db_lock);
	return ok;
}

int library_remote_count(const char *source) {
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *s = NULL;
	int count = 0;
	if (db && sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM REMOTE_MEDIA WHERE source=?", -1, &s, NULL) == SQLITE_OK) {
		sqlite3_bind_text(s, 1, source, -1, SQLITE_TRANSIENT);
		if (sqlite3_step(s) == SQLITE_ROW) count = sqlite3_column_int(s, 0);
	}
	sqlite3_finalize(s);
	pthread_mutex_unlock(&db_lock);
	return count;
}

int library_remote_covers(const char *source, int offset, char ids[][128], int capacity) {
	pthread_mutex_lock(&db_lock);
	sqlite3_stmt *s = NULL;
	int count = -1;
	if (db && offset >= 0 && capacity > 0 && sqlite3_prepare_v2(db,
		"SELECT DISTINCT COALESCE(NULLIF(cover_id,''),album_id) AS cover FROM REMOTE_STAGE "
		"WHERE source=? AND COALESCE(NULLIF(cover_id,''),album_id)<>'' ORDER BY cover LIMIT ? OFFSET ?",
		-1, &s, NULL) == SQLITE_OK) {
		sqlite3_bind_text(s, 1, source, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(s, 2, capacity); sqlite3_bind_int(s, 3, offset);
		count = 0;
		int rc;
		while ((rc = sqlite3_step(s)) == SQLITE_ROW && count < capacity) {
			snprintf(ids[count++], 128, "%s", (const char *)sqlite3_column_text(s, 0));
		}
		if (rc != SQLITE_DONE) count = -1;
	}
	sqlite3_finalize(s);
	pthread_mutex_unlock(&db_lock);
	return count;
}
