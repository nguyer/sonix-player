// Included in library.c: all state belongs to the card's SQLite database.
static bool offline_bind_done(sqlite3_stmt *s) { bool ok=s && sqlite3_step(s)==SQLITE_DONE;sqlite3_finalize(s);return ok; }
bool library_offline_options(const char *source,offline_options_t *o) {
 *o=(offline_options_t){.mode=OFFLINE_SELECTIVE,.auto_cache=1,.interval_minutes=60,.max_bytes=4LL*1024*1024*1024,.reserve_bytes=512LL*1024*1024};
 pthread_mutex_lock(&db_lock);sqlite3_stmt *s=NULL;
 bool ok=db && sqlite3_prepare_v2(db,"SELECT mode,automatic,charging,cleanup,interval_minutes,max_bytes,reserve_bytes,last_run,auto_cache FROM SUBSONIC_OFFLINE_OPTIONS WHERE source=?",-1,&s,NULL)==SQLITE_OK;
 if(ok){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);int rc=sqlite3_step(s);
  if(rc==SQLITE_ROW){o->mode=sqlite3_column_int(s,0);o->automatic=sqlite3_column_int(s,1);o->charging_only=sqlite3_column_int(s,2);o->cleanup=sqlite3_column_int(s,3);o->interval_minutes=sqlite3_column_int(s,4);o->max_bytes=sqlite3_column_int64(s,5);o->reserve_bytes=sqlite3_column_int64(s,6);o->last_run=sqlite3_column_int64(s,7);o->auto_cache=sqlite3_column_int(s,8);}
  else if(rc!=SQLITE_DONE)ok=false;
 }
 sqlite3_finalize(s);pthread_mutex_unlock(&db_lock);return ok;
}
bool library_offline_save_options(const char *source,const offline_options_t *o) {
 if(o->mode<0 || o->mode>2 || o->interval_minutes<1 || o->max_bytes<0 || o->reserve_bytes<0)return false;
 pthread_mutex_lock(&db_lock);sqlite3_stmt *s=NULL;
 bool ok=db && !scan_running && sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO SUBSONIC_OFFLINE_OPTIONS(source,mode,automatic,charging,cleanup,interval_minutes,max_bytes,reserve_bytes,last_run,auto_cache) VALUES(?,?,?,?,?,?,?,?,0,?)",-1,&s,NULL)==SQLITE_OK;
 if(ok){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,2,o->mode);sqlite3_bind_int(s,3,o->automatic);sqlite3_bind_int(s,4,o->charging_only);sqlite3_bind_int(s,5,o->cleanup);sqlite3_bind_int(s,6,o->interval_minutes);sqlite3_bind_int64(s,7,o->max_bytes);sqlite3_bind_int64(s,8,o->reserve_bytes);sqlite3_bind_int(s,9,o->auto_cache);ok=offline_bind_done(s);}else sqlite3_finalize(s);
 pthread_mutex_unlock(&db_lock);return ok;
}
bool library_offline_complete(const char *source,int64_t when) {
 pthread_mutex_lock(&db_lock);sqlite3_stmt *s=NULL;
 bool ok=db && sqlite3_prepare_v2(db,"UPDATE SUBSONIC_OFFLINE_OPTIONS SET last_run=? WHERE source=?",-1,&s,NULL)==SQLITE_OK;
 if(ok){sqlite3_bind_int64(s,1,when);sqlite3_bind_text(s,2,source,-1,SQLITE_TRANSIENT);ok=offline_bind_done(s);}else sqlite3_finalize(s);
 pthread_mutex_unlock(&db_lock);return ok;
}
bool library_offline_rule(const char *source,int kind,const char *value,bool selected) {
 if(kind<1 || kind>7 || !value || !*value || strlen(value)>255)return false;
 pthread_mutex_lock(&db_lock);sqlite3_stmt *s=NULL;
 bool ok=db && !scan_running && sqlite3_prepare_v2(db,selected?"INSERT OR IGNORE INTO SUBSONIC_OFFLINE_RULES VALUES(?,?,?)":"DELETE FROM SUBSONIC_OFFLINE_RULES WHERE source=? AND kind=? AND value=?",-1,&s,NULL)==SQLITE_OK;
 if(ok){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,2,kind);sqlite3_bind_text(s,3,value,-1,SQLITE_TRANSIENT);ok=offline_bind_done(s);}else sqlite3_finalize(s);
 if(ok){s=NULL;if(sqlite3_prepare_v2(db,"UPDATE SUBSONIC_OFFLINE_OPTIONS SET last_run=0 WHERE source=?",-1,&s,NULL)==SQLITE_OK){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);offline_bind_done(s);}}
 pthread_mutex_unlock(&db_lock);return ok;
}
bool library_offline_has_rule(const char *source,int kind,const char *value) {
 pthread_mutex_lock(&db_lock);sqlite3_stmt *s=NULL;bool found=false;
 if(db && sqlite3_prepare_v2(db,"SELECT 1 FROM SUBSONIC_OFFLINE_RULES WHERE source=? AND kind=? AND value=?",-1,&s,NULL)==SQLITE_OK){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,2,kind);sqlite3_bind_text(s,3,value,-1,SQLITE_TRANSIENT);found=sqlite3_step(s)==SQLITE_ROW;}
 sqlite3_finalize(s);pthread_mutex_unlock(&db_lock);return found;
}
int library_offline_choices(const char *source,int kind,const char *filter,int offset,offline_choice_t *out,int max) {
 const char *select=NULL;
 switch(kind){
 case OFFLINE_SONG:select="SELECT remote_id AS value,title||' — '||artist AS label FROM REMOTE_MEDIA WHERE source=?";break;
 case OFFLINE_ALBUM:select="SELECT DISTINCT album_id AS value,album||' — '||COALESCE(NULLIF(album_artist,''),artist) AS label FROM REMOTE_MEDIA WHERE source=? AND album_id<>''";break;
 case OFFLINE_ARTIST:select="SELECT DISTINCT artist AS value,artist AS label FROM REMOTE_MEDIA WHERE source=? AND artist<>'' UNION SELECT DISTINCT album_artist,album_artist FROM REMOTE_MEDIA WHERE source=?1 AND album_artist<>''";break;
 case OFFLINE_PLAYLIST:select="SELECT id AS value,local_name AS label FROM REMOTE_PLAYLISTS WHERE source=?";break;
 default:return -1;
 }
 char sql[1800];snprintf(sql,sizeof(sql),"SELECT value,label FROM (%s) WHERE instr(lower(label),lower(?2))>0 ORDER BY label COLLATE listorder LIMIT ?3 OFFSET ?4",select);
 pthread_mutex_lock(&db_lock);sqlite3_stmt *s=NULL;int count=-1;
 if(db && sqlite3_prepare_v2(db,sql,-1,&s,NULL)==SQLITE_OK){
  sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,filter?filter:"",-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,3,max);sqlite3_bind_int(s,4,offset);
  count=0;int rc;
  while((rc=sqlite3_step(s))==SQLITE_ROW && count<max){
   memset(&out[count],0,sizeof(out[count]));snprintf(out[count].value,256,"%s",sqlite3_column_text(s,0));snprintf(out[count].label,256,"%s",sqlite3_column_text(s,1));++count;
  }
  if(rc!=SQLITE_DONE)count=-1;
 }
 sqlite3_finalize(s);pthread_mutex_unlock(&db_lock);
 for(int i=0;i<count;++i)out[i].selected=library_offline_has_rule(source,kind,out[i].value);
 return count;
}
bool library_offline_plan(const char *source,bool mirror,const char (*extra)[128],int extra_count) {
 pthread_mutex_lock(&db_lock);
 bool transaction=db && !scan_running && exec("BEGIN"),ok=transaction;
 if(ok)ok=exec("CREATE TEMP TABLE IF NOT EXISTS offline_plan(id TEXT PRIMARY KEY)") && exec("DELETE FROM offline_plan");
 sqlite3_stmt *s=NULL;
 const char *sql="INSERT OR IGNORE INTO offline_plan SELECT r.remote_id FROM REMOTE_MEDIA r WHERE r.source=? AND (?2 OR EXISTS("
  "SELECT 1 FROM SUBSONIC_OFFLINE_RULES q WHERE q.source=r.source AND ((q.kind=1 AND q.value=r.remote_id) OR "
  "(q.kind=2 AND q.value=r.album_id) OR (q.kind=3 AND (q.value=r.artist OR q.value=r.album_artist)) OR "
  "(q.kind=6 AND CAST(q.value AS INTEGER)>0 AND datetime(r.created)>=datetime('now','-'||q.value||' days')) OR "
  "(q.kind=7 AND EXISTS(SELECT 1 FROM PLAY_COUNTS p WHERE p.path=r.path AND p.last_played>=CAST(strftime('%s','now') AS INTEGER)-CAST(q.value AS INTEGER)*86400)))))";
 if(ok)ok=sqlite3_prepare_v2(db,sql,-1,&s,NULL)==SQLITE_OK;
 if(ok){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,2,mirror);ok=offline_bind_done(s);s=NULL;}
 if(ok)ok=sqlite3_prepare_v2(db,"INSERT OR IGNORE INTO offline_plan VALUES(?)",-1,&s,NULL)==SQLITE_OK;
 for(int i=0;ok && i<extra_count;++i){sqlite3_bind_text(s,1,extra[i],-1,SQLITE_TRANSIENT);ok=sqlite3_step(s)==SQLITE_DONE;sqlite3_reset(s);}
 sqlite3_finalize(s);s=NULL;
 if(ok)ok=sqlite3_prepare_v2(db,"SELECT count(*) FROM offline_plan p WHERE NOT EXISTS(SELECT 1 FROM REMOTE_MEDIA r WHERE r.source=? AND r.remote_id=p.id)",-1,&s,NULL)==SQLITE_OK;
 if(ok){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);ok=sqlite3_step(s)==SQLITE_ROW && sqlite3_column_int(s,0)==0;}
 sqlite3_finalize(s);s=NULL;
 if(ok && !mirror){
  ok=sqlite3_prepare_v2(db,"DELETE FROM offline_plan WHERE id IN(SELECT value FROM SUBSONIC_OFFLINE_RULES WHERE source=? AND kind=8)",-1,&s,NULL)==SQLITE_OK;
  if(ok){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);ok=offline_bind_done(s);s=NULL;}
 }
 if(ok)ok=sqlite3_prepare_v2(db,"UPDATE SUBSONIC_OFFLINE_FILES SET wanted=0 WHERE source=?",-1,&s,NULL)==SQLITE_OK;
 if(ok){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);ok=offline_bind_done(s);s=NULL;}
 if(ok)ok=sqlite3_prepare_v2(db,"INSERT INTO SUBSONIC_OFFLINE_FILES(source,id,path,wanted) SELECT source,remote_id,path,1 FROM REMOTE_MEDIA r WHERE source=? AND remote_id IN(SELECT id FROM offline_plan) "
  "ON CONFLICT(source,id) DO UPDATE SET path=excluded.path,wanted=1",-1,&s,NULL)==SQLITE_OK;
 if(ok){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);ok=offline_bind_done(s);s=NULL;}
 sqlite3_finalize(s);
 if(ok)ok=exec("COMMIT");
 if(!ok && transaction)exec("ROLLBACK");
 pthread_mutex_unlock(&db_lock);return ok;
}

bool library_offline_select(const char *source,int kind,const char *value,const char (*ids)[128],int count,bool selected){
 if(count<1 || !ids || (kind!=OFFLINE_SONG && kind!=OFFLINE_ALBUM && kind!=OFFLINE_PLAYLIST))return false;
 pthread_mutex_lock(&db_lock);
 bool transaction=db && !scan_running && exec("BEGIN"),ok=transaction;sqlite3_stmt *s=NULL;
 if(ok && kind!=OFFLINE_SONG){
  ok=value && *value && sqlite3_prepare_v2(db,selected?"INSERT OR IGNORE INTO SUBSONIC_OFFLINE_RULES VALUES(?,?,?)":"DELETE FROM SUBSONIC_OFFLINE_RULES WHERE source=? AND kind=? AND value=?",-1,&s,NULL)==SQLITE_OK;
  if(ok){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,2,kind);sqlite3_bind_text(s,3,value,-1,SQLITE_TRANSIENT);ok=offline_bind_done(s);s=NULL;}
 }
 for(int i=0;ok && i<count;++i){
  const char *sql=selected?"DELETE FROM SUBSONIC_OFFLINE_RULES WHERE source=? AND kind=8 AND value=?":"DELETE FROM SUBSONIC_OFFLINE_RULES WHERE source=? AND kind=1 AND value=?";
  ok=sqlite3_prepare_v2(db,sql,-1,&s,NULL)==SQLITE_OK;
  if(ok){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,ids[i],-1,SQLITE_TRANSIENT);ok=offline_bind_done(s);s=NULL;}
  if(ok && (!selected || kind==OFFLINE_SONG)){
   ok=sqlite3_prepare_v2(db,"INSERT OR IGNORE INTO SUBSONIC_OFFLINE_RULES VALUES(?,?,?)",-1,&s,NULL)==SQLITE_OK;
   if(ok){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,2,selected?OFFLINE_SONG:OFFLINE_EXCLUDE);sqlite3_bind_text(s,3,ids[i],-1,SQLITE_TRANSIENT);ok=offline_bind_done(s);s=NULL;}
  }
  if(ok && !selected){
   ok=sqlite3_prepare_v2(db,"UPDATE SUBSONIC_OFFLINE_FILES SET wanted=0 WHERE source=? AND id=?",-1,&s,NULL)==SQLITE_OK;
   if(ok){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,ids[i],-1,SQLITE_TRANSIENT);ok=offline_bind_done(s);s=NULL;}
  }
 }
 sqlite3_finalize(s);
 if(ok)ok=exec("COMMIT");
 if(!ok && transaction)exec("ROLLBACK");
 pthread_mutex_unlock(&db_lock);return ok;
}
bool library_offline_clear(const char *source){
 pthread_mutex_lock(&db_lock);bool transaction=db && !scan_running && exec("BEGIN"),ok=transaction;sqlite3_stmt *s=NULL;
 const char *sql[]={"DELETE FROM SUBSONIC_OFFLINE_RULES WHERE source=?","UPDATE SUBSONIC_OFFLINE_OPTIONS SET mode=1 WHERE source=?","UPDATE SUBSONIC_OFFLINE_FILES SET wanted=0 WHERE source=?"};
 for(int i=0;ok && i<3;++i){ok=sqlite3_prepare_v2(db,sql[i],-1,&s,NULL)==SQLITE_OK;
  if(ok){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);ok=offline_bind_done(s);s=NULL;}}
 sqlite3_finalize(s);if(ok)ok=exec("COMMIT");if(!ok && transaction)exec("ROLLBACK");
 pthread_mutex_unlock(&db_lock);return ok;
}
int library_offline_files(const char *source,int offset,offline_file_t *out,int max) {
 pthread_mutex_lock(&db_lock);sqlite3_stmt *s=NULL;int count=-1;
 if(db && sqlite3_prepare_v2(db,"SELECT f.id,f.path,f.bytes,f.stamp,f.wanted,f.status,COALESCE(r.file_size,0),r.path IS NOT NULL FROM SUBSONIC_OFFLINE_FILES f LEFT JOIN REMOTE_MEDIA r ON r.path=f.path AND r.source=f.source WHERE f.source=? ORDER BY f.id LIMIT ? OFFSET ?",-1,&s,NULL)==SQLITE_OK){
  sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,2,max);sqlite3_bind_int(s,3,offset);count=0;int rc;
  while((rc=sqlite3_step(s))==SQLITE_ROW && count<max){offline_file_t *f=&out[count++];memset(f,0,sizeof(*f));snprintf(f->id,128,"%s",sqlite3_column_text(s,0));snprintf(f->path,512,"%s",sqlite3_column_text(s,1));f->bytes=sqlite3_column_int64(s,2);f->stamp=sqlite3_column_int64(s,3);f->wanted=sqlite3_column_int(s,4);f->status=sqlite3_column_int(s,5);f->remote_bytes=sqlite3_column_int64(s,6);f->catalog_present=sqlite3_column_int(s,7)!=0;}
  if(rc!=SQLITE_DONE)count=-1;
 }
 sqlite3_finalize(s);pthread_mutex_unlock(&db_lock);return count;
}
bool library_offline_note(const char *source,const char *id,int64_t bytes,int64_t stamp,int status) {
 pthread_mutex_lock(&db_lock);sqlite3_stmt *s=NULL;
 bool ok=db && sqlite3_prepare_v2(db,"UPDATE SUBSONIC_OFFLINE_FILES SET bytes=?,stamp=?,status=? WHERE source=? AND id=?",-1,&s,NULL)==SQLITE_OK;
 if(ok){sqlite3_bind_int64(s,1,bytes);sqlite3_bind_int64(s,2,stamp);sqlite3_bind_int(s,3,status);sqlite3_bind_text(s,4,source,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,5,id,-1,SQLITE_TRANSIENT);ok=offline_bind_done(s);}else sqlite3_finalize(s);
 pthread_mutex_unlock(&db_lock);return ok;
}
bool library_offline_forget(const char *source,const char *id) {
 pthread_mutex_lock(&db_lock);sqlite3_stmt *s=NULL;
 bool ok=db && sqlite3_prepare_v2(db,"DELETE FROM SUBSONIC_OFFLINE_FILES WHERE source=? AND id=?",-1,&s,NULL)==SQLITE_OK;
 if(ok){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,id,-1,SQLITE_TRANSIENT);ok=offline_bind_done(s);}else sqlite3_finalize(s);
 pthread_mutex_unlock(&db_lock);return ok;
}
int64_t library_offline_bytes(const char *source) {
 pthread_mutex_lock(&db_lock);sqlite3_stmt *s=NULL;int64_t bytes=0;
 if(db && sqlite3_prepare_v2(db,"SELECT COALESCE(SUM(bytes),0) FROM SUBSONIC_OFFLINE_FILES WHERE source=?",-1,&s,NULL)==SQLITE_OK){sqlite3_bind_text(s,1,source,-1,SQLITE_TRANSIENT);if(sqlite3_step(s)==SQLITE_ROW)bytes=sqlite3_column_int64(s,0);}
 sqlite3_finalize(s);pthread_mutex_unlock(&db_lock);return bytes;
}
bool library_offline_path_protected(const char *path) {
 pthread_mutex_lock(&db_lock);sqlite3_stmt *s=NULL;bool protected=true;
 if(db && sqlite3_prepare_v2(db,"SELECT EXISTS(SELECT 1 FROM PLAYBACK_QUEUE WHERE path=?) OR EXISTS(SELECT 1 FROM PLAYBACK_STATE WHERE path=?1) OR EXISTS(SELECT 1 FROM PLAYBACK_QUEUE_QUERY)",-1,&s,NULL)==SQLITE_OK){sqlite3_bind_text(s,1,path,-1,SQLITE_TRANSIENT);if(sqlite3_step(s)==SQLITE_ROW)protected=sqlite3_column_int(s,0)!=0;}
 sqlite3_finalize(s);pthread_mutex_unlock(&db_lock);return protected;
}

bool library_offline_wanted(const char *path){
 pthread_mutex_lock(&db_lock);sqlite3_stmt *s=NULL;bool wanted=false;
 if(db && sqlite3_prepare_v2(db,"SELECT 1 FROM SUBSONIC_OFFLINE_FILES WHERE path=? AND wanted=1",-1,&s,NULL)==SQLITE_OK){
  sqlite3_bind_text(s,1,path,-1,SQLITE_TRANSIENT);wanted=sqlite3_step(s)==SQLITE_ROW;
 }
 sqlite3_finalize(s);pthread_mutex_unlock(&db_lock);return wanted;
}
