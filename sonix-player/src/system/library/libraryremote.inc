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


static void remote_clean_sql(sqlite3_context *context,int argc,sqlite3_value **argv){
 const char *text=argc?(const char *)sqlite3_value_text(argv[0]):NULL;
 char *copy=sqlite3_mprintf("%s",text?text:"");
 if(!copy){sqlite3_result_error_nomem(context);return;}
 subsonic_clean_text(copy);sqlite3_result_text(context,copy,-1,sqlite3_free);
}

static bool remote_restore(void) {
	bool ok = exec("UPDATE REMOTE_MEDIA SET album=remote_clean(album) WHERE album IS NOT remote_clean(album)")
  && exec("UPDATE REMOTE_MEDIA SET album_key='remote:'||source||':'||COALESCE(NULLIF(album_id,''),album) "
   "WHERE album_key IS NOT 'remote:'||source||':'||COALESCE(NULLIF(album_id,''),album)")
  && exec("INSERT INTO MEDIA_TABLE(id,path,name,album,artist,album_artist,genre,year,dis_id,disc,"
		"character,sortkey,album_key,size,mtime,ctime,cue_id) "
		"SELECT -r.rowid,r.path,r.title,r.album,r.artist,COALESCE(NULLIF(r.album_artist,''),r.artist),r.genre,r.year,"
		"r.track,MAX(1,r.disc),r.character,r.sortkey,r.album_key,"
		"0,0,0,0 FROM REMOTE_MEDIA r WHERE NOT EXISTS(SELECT 1 FROM MEDIA_TABLE m WHERE m.path=r.path)");
	// Restore both parts of album identity. Local tags may have trimmed the
    // title while the old server catalog retained trailing whitespace.
    ok = exec("UPDATE MEDIA_TABLE SET (album,album_key)=(SELECT r.album,r.album_key FROM REMOTE_MEDIA r WHERE r.path=MEDIA_TABLE.path) "
      "WHERE EXISTS(SELECT 1 FROM REMOTE_MEDIA r WHERE r.path=MEDIA_TABLE.path "
      "AND (MEDIA_TABLE.album IS NOT r.album OR MEDIA_TABLE.album_key IS NOT r.album_key))") && ok;
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
	if (ok) ok = sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO REMOTE_STAGE VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
		-1, &s, NULL) == SQLITE_OK;
	for (int i = 0; ok && i < count; ++i) {
		const library_remote_track_t *r = &rows[i];
        subsonic_song_t song=r->song;subsonic_song_normalize(&song);
		char sort[LIBRARY_SORT_KEY_MAX], character[16], akey[sizeof(r->source)+sizeof(song.album)+8];
		int group;
		library_sort_key(song.title, sort, sizeof(sort));
		index_character(song.title, character, sizeof(character), &group);
		snprintf(akey, sizeof(akey), "remote:%s:%s", r->source, song.album_id[0] ? song.album_id : song.album);
		const char *values[] = {r->path,r->source,r->server,r->username,song.id,song.album_id,
			song.title,song.artist,song.album,song.cover_id};
		for (int k = 0; k < 10; ++k) sqlite3_bind_text(s, k + 1, values[k], -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(s, 11, song.duration); sqlite3_bind_int(s, 12, song.track);
		sqlite3_bind_text(s, 13, song.suffix, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(s, 14, sort, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(s, 15, character, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(s, 16, akey, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(s, 17, song.album_artist, -1, SQLITE_TRANSIENT);
		sqlite3_bind_text(s, 18, song.genre, -1, SQLITE_TRANSIENT);
		sqlite3_bind_int(s, 19, song.year); sqlite3_bind_int(s, 20, song.disc);
		sqlite3_bind_int64(s,21,song.size_bytes);sqlite3_bind_text(s,22,song.created,-1,SQLITE_TRANSIENT);
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
        "DELETE FROM REMOTE_REMOVED WHERE source=? AND path IN(SELECT path FROM REMOTE_STAGE WHERE source=?1)",
        "INSERT OR IGNORE INTO REMOTE_REMOVED SELECT source,path,remote_id FROM REMOTE_MEDIA r WHERE source=? AND NOT EXISTS(SELECT 1 FROM REMOTE_STAGE s WHERE s.source=r.source AND s.path=r.path)",
		"DELETE FROM MEDIA_TABLE WHERE path IN (SELECT path FROM REMOTE_MEDIA WHERE source=?)",
		"DELETE FROM REMOTE_MEDIA WHERE source=?",
		"INSERT INTO REMOTE_MEDIA SELECT * FROM REMOTE_STAGE WHERE source=?",
		"DELETE FROM REMOTE_STAGE WHERE source=?"};
	for (size_t i = 0; ok && i < sizeof(sql)/sizeof(sql[0]); ++i) {
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
		r->song.size_bytes=sqlite3_column_int64(s,20);
		const char *created=(const char *)sqlite3_column_text(s,21);
		snprintf(r->song.created,sizeof(r->song.created),"%s",created?created:"");
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

bool library_remote_metadata(const char *path,song_metadata_t *metadata){
 if(!path || !metadata || !strstr(path,"/.local/subsonic-cache/"))return false;
 library_remote_track_t track;if(!library_remote_get(path,&track))return false;
 if(!metadata->title[0])snprintf(metadata->title,sizeof(metadata->title),"%s",track.song.title);
 if(!metadata->artist[0])snprintf(metadata->artist,sizeof(metadata->artist),"%s",track.song.artist);
 if(!metadata->album_artist[0])snprintf(metadata->album_artist,sizeof(metadata->album_artist),"%s",track.song.album_artist);
 if(!metadata->album[0])snprintf(metadata->album,sizeof(metadata->album),"%s",track.song.album);
 if(!metadata->genre[0])snprintf(metadata->genre,sizeof(metadata->genre),"%s",track.song.genre);
 if(!metadata->track_number)metadata->track_number=track.song.track;
 if(!metadata->disc_number)metadata->disc_number=track.song.disc;
 if(!metadata->year)metadata->year=track.song.year;
 metadata->has_tags=metadata->has_tags || metadata->title[0] || metadata->artist[0] || metadata->album[0] || metadata->genre[0];
 return true;
}

int library_remote_removed(const char *source,int offset,library_remote_removed_t *rows,int capacity){
 pthread_mutex_lock(&db_lock);sqlite3_stmt *s=NULL;int count=-1;
 if(db && sqlite3_prepare_v2(db,"SELECT path,remote_id FROM REMOTE_REMOVED WHERE source=? ORDER BY path LIMIT ? OFFSET ?",-1,&s,NULL)==SQLITE_OK){
  sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,2,capacity);sqlite3_bind_int(s,3,offset);count=0;int rc;
  while((rc=sqlite3_step(s))==SQLITE_ROW && count<capacity){
   snprintf(rows[count].path,512,"%s",sqlite3_column_text(s,0));snprintf(rows[count].id,128,"%s",sqlite3_column_text(s,1));++count;
  }
  if(rc!=SQLITE_DONE)count=-1;
 }
 sqlite3_finalize(s);pthread_mutex_unlock(&db_lock);return count;
}
bool library_remote_removed_pending(const char *source,const char *keep){
 pthread_mutex_lock(&db_lock);sqlite3_stmt *s=NULL;bool pending=false;
 if(db && sqlite3_prepare_v2(db,"SELECT 1 FROM REMOTE_REMOVED WHERE source=? AND path<>? LIMIT 1",-1,&s,NULL)==SQLITE_OK){
  sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,keep?keep:"",-1,SQLITE_TRANSIENT);pending=sqlite3_step(s)==SQLITE_ROW;
 }
 sqlite3_finalize(s);pthread_mutex_unlock(&db_lock);return pending;
}
bool library_remote_removed_done(const char *source,const char *path){
 pthread_mutex_lock(&db_lock);bool transaction=db && exec("BEGIN"),ok=transaction;sqlite3_stmt *s=NULL;
 const char *sql[]={"DELETE FROM SUBSONIC_OFFLINE_FILES WHERE source=? AND path=?","DELETE FROM REMOTE_REMOVED WHERE source=? AND path=?"};
 for(int i=0;ok && i<2;++i){
  ok=sqlite3_prepare_v2(db,sql[i],-1,&s,NULL)==SQLITE_OK;
  if(ok){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,path,-1,SQLITE_TRANSIENT);ok=sqlite3_step(s)==SQLITE_DONE;}
  sqlite3_finalize(s);s=NULL;
 }
 if(ok)ok=exec("COMMIT");
 if(!ok && transaction)exec("ROLLBACK");
 pthread_mutex_unlock(&db_lock);return ok;
}
