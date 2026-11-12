// Included after the playlist SQL helpers; every operation uses db_lock.

// Only server-linked tables get these triggers. Local-only mutations never
// create pending work. Dirty markers live in SQLite and survive process exit.
static bool playlist_auto_triggers_locked(const library_playlist_link_t *link) {
 char quoted[PLAYLIST_TABLE_MAX];
 if(!playlist_table(link->local_name,quoted,sizeof(quoted)) || !playlist_table_exists_locked(link->local_name)) return false;
 sqlite3_stmt *s=NULL; sqlite3_int64 rowid=0;
 if(sqlite3_prepare_v2(db,"SELECT rowid FROM REMOTE_PLAYLISTS WHERE source=? AND id=?",-1,&s,NULL)==SQLITE_OK) {
  sqlite3_bind_text(s,1,link->source,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,link->id,-1,SQLITE_TRANSIENT);
  if(sqlite3_step(s)==SQLITE_ROW)rowid=sqlite3_column_int64(s,0);
 }
 sqlite3_finalize(s);if(!rowid)return false;
 const char *events[]={"INSERT","UPDATE","DELETE"};
 for(int i=0;i<3;++i) {
  char *sql=sqlite3_mprintf("CREATE TRIGGER IF NOT EXISTS \"remote_playlist_%lld_%d\" AFTER %s ON %s BEGIN "
   "INSERT INTO REMOTE_PLAYLIST_DIRTY VALUES(%Q,%Q,CAST(strftime('%%s','now') AS INTEGER)) "
   "ON CONFLICT(source,id) DO UPDATE SET changed_at=excluded.changed_at; END",
   rowid,i,events[i],quoted,link->source,link->id);
  bool ok=sql && exec(sql);sqlite3_free(sql);if(!ok)return false;
 }
 return true;
}
static void playlist_auto_mark_name_locked(const char *name) {
 sqlite3_stmt *s=NULL;
 if(db && sqlite3_prepare_v2(db,"INSERT INTO REMOTE_PLAYLIST_DIRTY SELECT source,id,CAST(strftime('%s','now') AS INTEGER) "
  "FROM REMOTE_PLAYLISTS WHERE local_name=? COLLATE NOCASE "
  "ON CONFLICT(source,id) DO UPDATE SET changed_at=excluded.changed_at",-1,&s,NULL)==SQLITE_OK) {
  sqlite3_bind_text(s,1,name,-1,SQLITE_TRANSIENT);sqlite3_step(s);
 }
 sqlite3_finalize(s);
}
static bool playlist_auto_clear_locked(const library_playlist_link_t *link) {
 sqlite3_stmt *s=NULL;
 bool ok=sqlite3_prepare_v2(db,"DELETE FROM REMOTE_PLAYLIST_DIRTY WHERE source=? AND id=?",-1,&s,NULL)==SQLITE_OK;
 if(ok){sqlite3_bind_text(s,1,link->source,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,link->id,-1,SQLITE_TRANSIENT);ok=sqlite3_step(s)==SQLITE_DONE;}
 sqlite3_finalize(s);return ok;
}
static void playlist_auto_setup_locked(void) {
 sqlite3_stmt *s=NULL;
 if(sqlite3_prepare_v2(db,"SELECT rowid,source,id,local_name FROM REMOTE_PLAYLISTS",-1,&s,NULL)!=SQLITE_OK)return;
 while(sqlite3_step(s)==SQLITE_ROW) {
  library_playlist_link_t link={0};
  snprintf(link.source,sizeof(link.source),"%s",sqlite3_column_text(s,1));
  snprintf(link.id,sizeof(link.id),"%s",sqlite3_column_text(s,2));
  snprintf(link.local_name,sizeof(link.local_name),"%s",sqlite3_column_text(s,3));
  char trigger[80];snprintf(trigger,sizeof(trigger),"remote_playlist_%lld_0",(long long)sqlite3_column_int64(s,0));
  sqlite3_stmt *check=NULL;bool known=false;
  if(sqlite3_prepare_v2(db,"SELECT 1 FROM sqlite_master WHERE type='trigger' AND name=?",-1,&check,NULL)==SQLITE_OK) {
   sqlite3_bind_text(check,1,trigger,-1,SQLITE_TRANSIENT);known=sqlite3_step(check)==SQLITE_ROW;
  }
  sqlite3_finalize(check);
  bool exists=playlist_table_exists_locked(link.local_name);
  if(exists)playlist_auto_triggers_locked(&link);
  // Once on upgrade, reconcile edits made before automatic synchronization.
  if(!known || !exists)playlist_auto_mark_name_locked(link.local_name);
 }
 sqlite3_finalize(s);
}
bool library_playlist_auto_pending(const char *source,int quiet_seconds) {
 pthread_mutex_lock(&db_lock);sqlite3_stmt *s=NULL;bool pending=false;
 if(db && sqlite3_prepare_v2(db,"SELECT 1 FROM REMOTE_PLAYLIST_DIRTY d JOIN REMOTE_PLAYLISTS p USING(source,id) "
  "WHERE source=? AND (changed_at<=CAST(strftime('%s','now') AS INTEGER)-? "
  "OR changed_at>CAST(strftime('%s','now') AS INTEGER)+60) LIMIT 1",-1,&s,NULL)==SQLITE_OK) {
  sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,2,quiet_seconds);pending=sqlite3_step(s)==SQLITE_ROW;
 }
 sqlite3_finalize(s);pthread_mutex_unlock(&db_lock);return pending;
}
bool library_playlist_auto_link_pending(const char *source,const char *id) {
 pthread_mutex_lock(&db_lock);sqlite3_stmt *s=NULL;bool pending=false;
 if(db && sqlite3_prepare_v2(db,"SELECT 1 FROM REMOTE_PLAYLIST_DIRTY WHERE source=? AND id=?",-1,&s,NULL)==SQLITE_OK) {
  sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,id,-1,SQLITE_TRANSIENT);pending=sqlite3_step(s)==SQLITE_ROW;
 }
 sqlite3_finalize(s);pthread_mutex_unlock(&db_lock);return pending;
}

static void playlist_link_read(sqlite3_stmt *s, library_playlist_link_t *r) {
	memset(r,0,sizeof(*r));
	char *fields[]={r->source,r->id,r->local_name,r->base_local,r->remote_name,r->hash};
	size_t sizes[]={33,128,201,201,256,33};
	for(int i=0;i<6;++i) { const char *v=(const char *)sqlite3_column_text(s,i); snprintf(fields[i],sizes[i],"%s",v?v:""); }
}
static bool playlist_link_write(const library_playlist_link_t *r) {
	sqlite3_stmt *s=NULL;
	bool ok=sqlite3_prepare_v2(db,"INSERT INTO REMOTE_PLAYLISTS VALUES(?,?,?,?,?,?) "
		"ON CONFLICT(source,id) DO UPDATE SET local_name=excluded.local_name,base_local=excluded.base_local,"
		"remote_name=excluded.remote_name,hash=excluded.hash",-1,&s,NULL)==SQLITE_OK;
	const char *fields[]={r->source,r->id,r->local_name,r->base_local,r->remote_name,r->hash};
	if(ok) { for(int i=0;i<6;++i) sqlite3_bind_text(s,i+1,fields[i],-1,SQLITE_TRANSIENT); ok=sqlite3_step(s)==SQLITE_DONE; }
	sqlite3_finalize(s);
	if(ok)ok=playlist_auto_triggers_locked(r) && playlist_auto_clear_locked(r);
	return ok;
}
int library_playlist_links(const char *source, library_playlist_link_t **out) {
	*out=NULL; int count=0,capacity=0,rc=SQLITE_ERROR;
	pthread_mutex_lock(&db_lock); sqlite3_stmt *s=NULL;
	bool ok=db && sqlite3_prepare_v2(db,"SELECT * FROM REMOTE_PLAYLISTS WHERE source=?",-1,&s,NULL)==SQLITE_OK;
	if(ok) sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);
	while(ok && (rc=sqlite3_step(s))==SQLITE_ROW) {
		if(count==capacity) { int cap=capacity?capacity*2:8; void *p=realloc(*out,(size_t)cap*sizeof(**out)); if(!p){ok=false;break;} *out=p;capacity=cap; }
		playlist_link_read(s,&(*out)[count++]);
	}
	sqlite3_finalize(s); pthread_mutex_unlock(&db_lock);
	if(!ok || rc!=SQLITE_DONE) {free(*out);*out=NULL;return -1;} return count;
}
bool library_playlist_link(const char *name, library_playlist_link_t *out) {
	pthread_mutex_lock(&db_lock); sqlite3_stmt *s=NULL;
	bool ok=db && sqlite3_prepare_v2(db,"SELECT * FROM REMOTE_PLAYLISTS WHERE local_name=? COLLATE NOCASE",-1,&s,NULL)==SQLITE_OK;
	if(ok){sqlite3_bind_text(s,1,name,-1,SQLITE_TRANSIENT);ok=sqlite3_step(s)==SQLITE_ROW;}
	if(ok) playlist_link_read(s,out);
	sqlite3_finalize(s);pthread_mutex_unlock(&db_lock);return ok;
}
bool library_playlist_link_record(const library_playlist_link_t *r,unsigned revision) {
	pthread_mutex_lock(&db_lock);
	bool ok=db && !scan_running && generation[GEN_PLAYLISTS]==revision && playlist_link_write(r);
	pthread_mutex_unlock(&db_lock);return ok;
}
bool library_playlist_remote_ids(const char *source,const char *name,char (**ids)[128],int *count,unsigned *revision) {
	*ids=NULL;*count=0; char table[PLAYLIST_TABLE_MAX],sql[PLAYLIST_TABLE_MAX+200];
	if(!playlist_table(name,table,sizeof(table))) return false;
	pthread_mutex_lock(&db_lock); sqlite3_stmt *s=NULL;
	bool ok=db && !scan_running && playlist_table_exists_locked(name);
	int n=0,rc=SQLITE_ERROR;
	snprintf(sql,sizeof(sql),"SELECT r.remote_id FROM %s p LEFT JOIN REMOTE_MEDIA r ON r.path=p.path AND r.source=? ORDER BY p.idx",table);
	if(ok) ok=sqlite3_prepare_v2(db,sql,-1,&s,NULL)==SQLITE_OK;
	int capacity=0;
	if(ok) sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);
	while(ok && (rc=sqlite3_step(s))==SQLITE_ROW) {
		const char *id=(const char *)sqlite3_column_text(s,0);
		if(!id || !*id || strlen(id)>=128 || n>=10000) {ok=false;break;}
		if(n==capacity) {int cap=capacity?capacity*2:32;void *p=realloc(*ids,(size_t)cap*128);if(!p){ok=false;break;}*ids=p;capacity=cap;}
		memset((*ids)[n],0,128);strcpy((*ids)[n++],id);
	}
	if(rc!=SQLITE_DONE) ok=false;
	*revision=generation[GEN_PLAYLISTS];
	sqlite3_finalize(s);pthread_mutex_unlock(&db_lock);
	if(!ok){free(*ids);*ids=NULL;return false;} *count=n;return true;
}
bool library_playlist_remote_replace(const library_playlist_link_t *link,const char *old,
	const char (*ids)[128],int count,unsigned revision) {
	char table[PLAYLIST_TABLE_MAX],from[PLAYLIST_TABLE_MAX],sql[2*PLAYLIST_TABLE_MAX+100];
	if(count<0 || !playlist_table(link->local_name,table,sizeof(table))) return false;
	pthread_mutex_lock(&db_lock);
	bool transaction=db && !scan_running && generation[GEN_PLAYLISTS]==revision && exec("BEGIN");
	bool ok=transaction;
	if(ok && (!old || strcmp(old,link->local_name))) {
		ok=!playlist_table_exists_locked(link->local_name);
		if(ok && old) {
			ok=playlist_table(old,from,sizeof(from)) && playlist_table_exists_locked(old);
			if(ok){snprintf(sql,sizeof(sql),"ALTER TABLE %s RENAME TO %s",from,table);ok=exec(sql);}
		}
	}
	if(ok) ok=playlist_create_locked(table);
	if(ok){snprintf(sql,sizeof(sql),"DELETE FROM %s",table);ok=exec(sql);}
	sqlite3_stmt *s=NULL;
	if(ok) ok=sqlite3_prepare_v2(db,"SELECT path,title,artist,duration FROM REMOTE_MEDIA WHERE source=? AND remote_id=? LIMIT 1",-1,&s,NULL)==SQLITE_OK;
	for(int i=0;ok && i<count;++i) {
		sqlite3_bind_text(s,1,link->source,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,ids[i],-1,SQLITE_TRANSIENT);
		ok=sqlite3_step(s)==SQLITE_ROW;
		if(ok) {
			library_playlist_row_t row={0};
			snprintf(row.path,sizeof(row.path),"%s",sqlite3_column_text(s,0));
			snprintf(row.title,sizeof(row.title),"%s",sqlite3_column_text(s,1));
			snprintf(row.artist,sizeof(row.artist),"%s",sqlite3_column_text(s,2));
			row.seconds=sqlite3_column_int(s,3);row.present=true;
			ok=playlist_append_locked(table,&row);
		}
		sqlite3_reset(s);
	}
	sqlite3_finalize(s);
	if(ok) ok=playlist_link_write(link);
	if(ok) ok=exec("COMMIT");
	if(!ok && transaction) exec("ROLLBACK");
	if(ok) bump_generation(GEN_PLAYLISTS);
	pthread_mutex_unlock(&db_lock);return ok;
}
bool library_playlist_link_drop(const library_playlist_link_t *link,bool drop,unsigned revision) {
	pthread_mutex_lock(&db_lock);
	bool transaction=db && !scan_running && generation[GEN_PLAYLISTS]==revision && exec("BEGIN");
	bool ok=transaction;
	if(ok && drop) {
		char table[PLAYLIST_TABLE_MAX],sql[PLAYLIST_TABLE_MAX+40];
		ok=playlist_table(link->local_name,table,sizeof(table));
		if(ok){snprintf(sql,sizeof(sql),"DROP TABLE IF EXISTS %s",table);ok=exec(sql);}
	}
	sqlite3_stmt *s=NULL;
	if(ok) ok=sqlite3_prepare_v2(db,"DELETE FROM REMOTE_PLAYLISTS WHERE source=? AND id=?",-1,&s,NULL)==SQLITE_OK;
	if(ok){sqlite3_bind_text(s,1,link->source,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,link->id,-1,SQLITE_TRANSIENT);ok=sqlite3_step(s)==SQLITE_DONE;}
	sqlite3_finalize(s);
	if(ok)ok=playlist_auto_clear_locked(link);
	if(ok) ok=exec("COMMIT");
	if(!ok && transaction) exec("ROLLBACK");
	if(ok) bump_generation(GEN_PLAYLISTS);
	pthread_mutex_unlock(&db_lock);return ok;
}
