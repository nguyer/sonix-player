#include "src/gui/library/medialist.h"
#include "src/gui/nowplaying/trackmenu.h"
#include "src/gui/shell/popover.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include "src/system/core/config.h"
#include "src/system/library/library.h"
#include "src/system/library/libraryremote.h"
#include "src/system/streaming/subsonicaccount.h"
#include "src/system/streaming/subsoniccache.h"
#include "src/system/streaming/subsoniccatalog.h"
#include "src/system/net/http.h"
#include "src/system/streaming/subsonicart.h"
#include "src/system/streaming/subsonicplaylistapi.h"
#include "src/system/library/albumart.h"
#include "src/system/library/playlists.h"
#include "src/system/db/sqlite3.h"
#include "src/gui/streaming/subsonicpage.c"

static atomic_bool measure_unchanged;
static atomic_uint measured_fetches,measured_notes,measured_sums;
bool __real_subsoniccache_fetch_offline(subsonic_client_t *,const subsonic_song_t *,const char *,char *,size_t,int64_t,int64_t,bool,const atomic_bool *);
bool __wrap_subsoniccache_fetch_offline(subsonic_client_t *c,const subsonic_song_t *song,const char *root,char *path,size_t size,int64_t allowance,int64_t reserve,bool force,const atomic_bool *cancel){
 if(atomic_load(&measure_unchanged))atomic_fetch_add(&measured_fetches,1);
 return __real_subsoniccache_fetch_offline(c,song,root,path,size,allowance,reserve,force,cancel);
}
bool __real_library_offline_note(const char *,const char *,int64_t,int64_t,int);
bool __wrap_library_offline_note(const char *source,const char *id,int64_t bytes,int64_t stamp,int status){
 if(atomic_load(&measure_unchanged))atomic_fetch_add(&measured_notes,1);
 return __real_library_offline_note(source,id,bytes,stamp,status);
}
int64_t __real_library_offline_bytes(const char *);
int64_t __wrap_library_offline_bytes(const char *source){
 if(atomic_load(&measure_unchanged))atomic_fetch_add(&measured_sums,1);
 return __real_library_offline_bytes(source);
}
static pthread_mutex_t availability_probe_lock=PTHREAD_MUTEX_INITIALIZER;
static char availability_probe_album[256];
static unsigned availability_probe_queries;
library_index_t *__real_library_index_open(library_list_t,library_filter_t,const char *,library_order_t,bool);
library_index_t *__wrap_library_index_open(library_list_t kind,library_filter_t filter,const char *value,library_order_t order,bool desc){
 pthread_mutex_lock(&availability_probe_lock);
 if(kind==LIBRARY_LIST_TRACKS && filter==LIBRARY_FILTER_ALBUM && value && availability_probe_album[0] && !strcmp(value,availability_probe_album))++availability_probe_queries;
 pthread_mutex_unlock(&availability_probe_lock);
 return __real_library_index_open(kind,filter,value,order,desc);
}
static void wait_album_available(const char *album,bool expected){
 for(int i=0;i<500;++i){subsonicavailability_refresh();if(subsonic_album_available(album)==expected)return;usleep(10000);}
 assert(!"album availability background lookup timed out");
}
static char test_current_path[512];
void __real_device_state_get(device_state_t *);
void __wrap_device_state_get(device_state_t *state){
 __real_device_state_get(state);
 if(test_current_path[0])snprintf(state->current_file,sizeof(state->current_file),"%s",test_current_path);
}
static audio_status_t test_audio_status=AUDIO_STATUS_PLAYING;
audio_status_t __real_audio_get_status(void);
audio_status_t __wrap_audio_get_status(void){return test_current_path[0]?test_audio_status:__real_audio_get_status();}
void __real_audio_get_current_file(char *,size_t);
void __wrap_audio_get_current_file(char *out,size_t size){
 if(test_current_path[0])snprintf(out,size,"%s",test_current_path);else __real_audio_get_current_file(out,size);
}
static int resumed_index = -1;
static int automatic_sync_calls,background_notices;
int __real_subsonicplaylists_sync_pending(subsonic_client_t *,const atomic_bool *);
int __wrap_subsonicplaylists_sync_pending(subsonic_client_t *c,const atomic_bool *cancel) {
 ++automatic_sync_calls;return __real_subsonicplaylists_sync_pending(c,cancel);
}
void __wrap_gui_notify_popup(const char *text) {(void)text;++background_notices;}
bool __wrap_device_state_play_queue_index(int index) { resumed_index = index; return true; }
void __wrap_player_refresh_now_playing(void) {}

static void pump_request(void) {
	for (int i = 0; i < 5000 && (busy || waiting_path[0]); ++i) { gui_post_drain(); usleep(1000); }
	assert(!busy && !waiting_path[0]);
}

static void wait_scan(void) {
	for (int i = 0; i < 15000 && library_scan_running(); ++i) usleep(1000);
	assert(!library_scan_running());
}

static int album_group_count(void) {
	library_index_t *ix = library_index_open(LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL, LIBRARY_ORDER_DEFAULT, false);
	assert(ix);
	int count = library_index_count(ix);
	library_index_close(ix);
	return count;
}

typedef struct { char values[10][512]; int count; } album_values_t;
static bool collect_album(const char *name, const char *path, const char *artist, void *user) {
	(void)artist;
	album_values_t *values = user;
	assert(name && *name); // a NULL album identity must never become "(no name)"
	assert(path && *path); // an empty phantom album has no representative track
	assert(values->count < 10);
	snprintf(values->values[values->count++], 512, "%s", name);
	return true;
}

static void assert_albums_populated(void) {
	album_values_t values = {0};
	library_index_t *ix = library_index_open(LIBRARY_LIST_ALBUMS, LIBRARY_FILTER_NONE, NULL, LIBRARY_ORDER_DEFAULT, false);
	assert(ix);
	library_index_window(ix, 0, 10, collect_album, &values);
	library_index_close(ix);
	for (int i = 0; i < values.count; ++i) {
		ix = library_index_open(LIBRARY_LIST_TRACKS, LIBRARY_FILTER_ALBUM, values.values[i], LIBRARY_ORDER_DEFAULT, false);
		assert(ix && library_index_count(ix) > 0);
		library_index_close(ix);
	}
}

static void control_server(const char *base,const char *action) {
	char url[640],*body;
	snprintf(url,sizeof(url),"%s/%s",base,action);
	assert(http_get(url,&body,NULL,1024,5));free(body);
}

static void test_playlists(subsonic_client_t *client,const char *root,const char *database) {
	atomic_bool cancel=false;
	library_playlist_row_t local={0};
	snprintf(local.path,sizeof(local.path),"%s/Local.wav",root);strcpy(local.title,"Local track");local.present=true;
	assert(library_playlist_create("Road"));assert(library_playlist_append("Road",&local));
	assert(library_playlist_create("Local only"));assert(library_playlist_append("Local only",&local));
	control_server(client->base_url,"test-playlist-list-fail");
	assert(subsonicplaylists_sync(client,&cancel)==-1);
	assert(library_playlist_count("Road")==1 && library_playlist_count("Local only")==1);
	assert(subsonicplaylists_sync(client,&cancel)==1);
	char source[33];subsoniccache_source(client,source);
	library_playlist_link_t *links=NULL,link;
	assert(library_playlist_links(source,&links)==1);link=links[0];free(links);
	assert(strcmp(link.local_name,"Road")); // collision never overwrites a local list
	assert(library_playlist_count("Road")==1 && library_playlist_count("Local only")==1);
	char (*ids)[128]=NULL;int count;unsigned rev;
	assert(library_playlist_remote_ids(source,link.local_name,&ids,&count,&rev));
	assert(count==3 && !strcmp(ids[0],"catalog-0") && !strcmp(ids[1],"catalog-1") && !strcmp(ids[2],"catalog-0"));free(ids);
	assert(!library_playlist_auto_pending(source,0));
	assert(library_playlist_move(link.local_name,0,2));
	assert(library_playlist_auto_pending(source,0));
	library_close();assert(library_open(database)); // pending offline edits survive restart
	assert(library_playlist_auto_pending(source,0));
	assert(subsonicplaylists_sync(client,&cancel)==1);
	subsonic_playlist_snapshot_t remote;
	assert(subsonic_playlist_get(client,"p1",&remote));
	assert(remote.count==3 && !strcmp(remote.ids[0],"catalog-1") && !strcmp(remote.ids[2],"catalog-0"));subsonic_playlist_free(&remote);
	assert(library_playlist_rename(link.local_name,"Driving"));
	assert(subsonicplaylists_sync(client,&cancel)==1);
	assert(subsonic_playlist_get(client,"p1",&remote));assert(!strcmp(remote.info.name,"Driving"));subsonic_playlist_free(&remote);
	control_server(client->base_url,"test-playlist-edit");
	assert(subsonicplaylists_sync(client,&cancel)==1);
	assert(!library_playlist_exists("Driving") && library_playlist_count("Server renamed")==2);
	library_remote_track_t song;char path[512];subsonic_song_t s={0};strcpy(s.id,"catalog-0");strcpy(s.suffix,"flac");
	assert(subsoniccache_path(client,&s,root,path,sizeof(path)) && library_remote_get(path,&song));
	library_playlist_row_t row={0};strcpy(row.path,path);strcpy(row.title,song.song.title);row.present=true;
	assert(library_playlist_append("Server renamed",&row));
	control_server(client->base_url,"test-playlist-conflict");
	assert(subsonicplaylists_sync(client,&cancel)==-1);
	assert(strstr(client->error,"both here and on the server"));
	assert(library_playlist_count("Server renamed")==3);
	assert(subsonic_playlist_get(client,"p1",&remote));assert(remote.count==1 && !strcmp(remote.ids[0],"catalog-3"));subsonic_playlist_free(&remote);
	int position=2;assert(library_playlist_remove_positions("Server renamed",&position,1)==1);
	assert(subsonicplaylists_sync(client,&cancel)==1);
	assert(library_playlist_count("Server renamed")==1);
	assert(library_playlist_drop("Server renamed"));
	library_close();assert(library_open(database)); // deletion remains pending across restart
	assert(subsonicplaylists_sync(client,&cancel)==1); // local deletion reaches server
	subsonic_playlist_t *lists=NULL;assert(subsonic_playlists(client,&lists)==0);free(lists);
	assert(library_playlist_create("Upload"));assert(library_playlist_append("Upload",&row));assert(library_playlist_append("Upload",&row));
	control_server(client->base_url,"test-playlist-upload-lost");
	assert(!subsonicplaylists_upload(client,"Upload",&cancel));
	assert(subsonicplaylists_upload(client,"Upload",&cancel));
	assert(library_playlist_link("Upload",&link));
	assert(subsonic_playlist_get(client,link.id,&remote));assert(remote.count==2);subsonic_playlist_free(&remote);
	assert(!subsonicplaylists_upload(client,"Local only",&cancel)); // no ID guessing or partial uploads
	assert(subsonic_playlists(client,&lists)==1);free(lists);
	library_close();
 sqlite3 *upgrade=NULL;sqlite3_stmt *upgrade_row=NULL;
 assert(sqlite3_open(database,&upgrade)==SQLITE_OK);
 assert(sqlite3_prepare_v2(upgrade,"SELECT rowid FROM REMOTE_PLAYLISTS WHERE local_name='Upload'",-1,&upgrade_row,NULL)==SQLITE_OK);
 assert(sqlite3_step(upgrade_row)==SQLITE_ROW);
 long long upgrade_id=sqlite3_column_int64(upgrade_row,0);sqlite3_finalize(upgrade_row);
 for(int i=0;i<3;++i) {
  char sql[128];snprintf(sql,sizeof(sql),"DROP TRIGGER remote_playlist_%lld_%d",upgrade_id,i);
  assert(sqlite3_exec(upgrade,sql,NULL,NULL,NULL)==SQLITE_OK);
 }
 sqlite3_close(upgrade);
 assert(library_open(database));assert(library_playlist_link("Upload",&link));
 assert(library_playlist_auto_pending(source,0)); // upgrade reconciles old server links once
 assert(subsonicplaylists_sync_pending(client,&cancel)==1);
 assert(!library_playlist_auto_pending(source,0));
	control_server(client->base_url,"test-playlist-delete");
	assert(subsonicplaylists_sync(client,&cancel)==0);
	assert(!library_playlist_exists("Upload"));
	assert(library_playlist_exists("Local only") && library_playlist_exists("Road"));
	assert(library_playlist_create("Empty"));
	assert(subsonicplaylists_upload(client,"Empty",&cancel));
	assert(library_playlist_link("Empty",&link));
	assert(subsonic_playlist_get(client,link.id,&remote));assert(remote.count==0);subsonic_playlist_free(&remote);
	assert(library_playlist_append("Empty",&row));assert(subsonicplaylists_sync(client,&cancel)==1);
	position=0;assert(library_playlist_remove_positions("Empty",&position,1)==1);
	assert(subsonicplaylists_sync(client,&cancel)==1);
	assert(subsonic_playlist_get(client,link.id,&remote));assert(remote.count==0);subsonic_playlist_free(&remote);
	assert(library_playlist_drop("Empty"));assert(subsonicplaylists_sync(client,&cancel)==1);
	control_server(client->base_url,"test-playlist-shared");
	assert(subsonicplaylists_sync(client,&cancel)==1);
	assert(library_playlist_append("Shared",&row));
	assert(subsonicplaylists_sync(client,&cancel)==-1);
	assert(strstr(client->error,"read-only"));
	assert(subsonic_playlist_get(client,"shared",&remote));assert(remote.count==1);subsonic_playlist_free(&remote);
	char (*large)[128]=calloc(500,128);assert(large);
	for(int i=0;i<500;++i)strcpy(large[i],"catalog-0");
	char large_id[128];
	assert(subsonic_playlist_put(client,NULL,"API large",(const char (*)[128])large,500,large_id));free(large);
	assert(subsonic_playlist_get(client,large_id,&remote));assert(remote.count==500);subsonic_playlist_free(&remote);
	assert(subsonic_playlist_delete(client,large_id));
	atomic_store(&cancel,true);assert(subsonicplaylists_sync(client,&cancel)==-1);
}


static void run_automatic_sync(void){
 // Let the scheduler's quiet-period/throttle gate actually open before checking results.
 for(int attempt=0;attempt<30 && !busy;++attempt){
  lv_tick_inc(1100);auto_sync_playlists();if(!busy)usleep(100000);
 }
 assert(busy);pump_request();
}
static void test_automatic_playlists(subsonic_client_t *client,const char *root,const char *database) {
 atomic_bool cancel=false;
 char source[33];subsoniccache_source(client,source);
 int position=1;assert(library_playlist_remove_positions("Shared",&position,1)==1);
 assert(subsonicplaylists_sync(client,&cancel)==1);
 assert(!library_playlist_auto_pending(source,0));
 playlists_init(root);
 char backup[600];snprintf(backup,sizeof(backup),"%s/Shared.m3u",root);
 FILE *backup_file=fopen(backup,"w");assert(backup_file);
 fprintf(backup_file,"#EXTM3U\n%s/Local.wav\n",root);fclose(backup_file);
 playlists_import_result_t imported;
 assert(playlists_import(backup,true,&imported));
 assert(strcmp(imported.name,"Shared") && library_playlist_count("Shared")==1);
 library_playlist_link_t imported_link;
 assert(!library_playlist_link(imported.name,&imported_link));
 assert(!library_playlist_auto_pending(source,0));
 library_playlist_row_t row={0};subsonic_song_t song={0};
 strcpy(song.id,"catalog-0");strcpy(song.suffix,"flac");
 assert(subsoniccache_path(client,&song,root,row.path,sizeof(row.path)));row.present=true;
 assert(library_playlist_create("Automatic"));
 assert(subsonicplaylists_upload(client,"Automatic",&cancel));
 library_playlist_link_t link;assert(library_playlist_link("Automatic",&link));
 assert(!library_playlist_auto_pending(source,0));
 int calls=automatic_sync_calls;
 assert(library_playlist_append("Local only",&row));
 auto_retry_at=0;lv_tick_inc(3000);auto_sync_playlists();
 assert(!busy && automatic_sync_calls==calls); // no network work for local-only edits
 assert(library_playlist_append("Automatic",&row));
 assert(library_playlist_auto_pending(source,0));
 library_close();assert(library_open(database));
 assert(library_playlist_auto_pending(source,0)); // persistent queue
 usleep(2100000);lv_tick_inc(3000);
 lv_obj_t *screen=lv_screen_active();
 run_automatic_sync();
 assert(lv_screen_active()==screen && automatic_sync_calls==calls+1);
 assert(!library_playlist_auto_pending(source,0));
 subsonic_playlist_snapshot_t remote;
 assert(subsonic_playlist_get(client,link.id,&remote));assert(remote.count==1);subsonic_playlist_free(&remote);
 assert(library_playlist_append("Automatic",&row));
 control_server(client->base_url,"test-playlist-list-fail");
 usleep(2100000);lv_tick_inc(3000);auto_retry_at=0;
 int notices=background_notices;
 run_automatic_sync();
 assert(library_playlist_auto_pending(source,0) && background_notices==notices+1);
 calls=automatic_sync_calls;lv_tick_inc(3000);auto_sync_playlists();
 assert(!busy && automatic_sync_calls==calls); // backoff, no tight retry loop
 auto_retry_at=0;lv_tick_inc(3000);run_automatic_sync();
 assert(!library_playlist_auto_pending(source,0));
 assert(subsonic_playlist_get(client,link.id,&remote));assert(remote.count==2);subsonic_playlist_free(&remote);
 // A conflicting/read-only playlist does not block other healthy server lists.
 assert(library_playlist_append("Shared",&row));assert(library_playlist_append("Automatic",&row));
 usleep(2100000);lv_tick_inc(3000);auto_retry_at=0;
 run_automatic_sync();
 assert(subsonic_playlist_get(client,link.id,&remote));assert(remote.count==3);subsonic_playlist_free(&remote);
 assert(!library_playlist_auto_link_pending(source,link.id));
 assert(library_playlist_auto_pending(source,0)); // Shared still needs resolution
 position=1;assert(library_playlist_remove_positions("Shared",&position,1)==1);
 assert(subsonicplaylists_sync(client,&cancel)==2);
 assert(!library_playlist_auto_pending(source,0));
 assert(library_playlist_drop("Automatic"));assert(library_playlist_auto_pending(source,0));
 usleep(2100000);lv_tick_inc(3000);auto_retry_at=0;
 run_automatic_sync();
 assert(!library_playlist_auto_pending(source,0));
 assert(!library_playlist_link("Automatic",&link));
 subsonic_playlist_t *lists=NULL;assert(subsonic_playlists(client,&lists)==1);free(lists);
 assert(library_playlist_exists("Local only")); // never uploaded or removed
}


static void test_offline(subsonic_client_t *client,const char *root,const char *local) {
 char source[33];subsoniccache_source(client,source);
 offline_options_t opt;assert(library_offline_options(source,&opt));
 opt.mode=OFFLINE_SELECTIVE;opt.reserve_bytes=0;opt.max_bytes=4096;
 assert(library_offline_save_options(source,&opt));
 assert(library_offline_rule(source,OFFLINE_SONG,"catalog-7",true));
 assert(library_offline_plan(source,false,NULL,0));
 offline_file_t files[10];assert(library_offline_files(source,0,files,10)==1);
 assert(!strcmp(files[0].id,"catalog-7") && files[0].wanted);
 char unknown[1][128]={"unknown-song"};
 assert(!library_offline_plan(source,false,unknown,1));
 assert(library_offline_files(source,0,files,10)==1 && files[0].wanted);
 assert(subsonicoffline_start(client,root,false));
 for(int i=0;i<1000 && subsonicoffline_busy();++i)usleep(10000);
 assert(!subsonicoffline_busy());
 subsonic_offline_status_t state;subsonicoffline_status(&state);
 fprintf(stderr,"offline test: %s\n",state.message);
 assert(library_offline_files(source,0,files,10)==1);
 assert(files[0].bytes>0 && files[0].status==2);
 char marker[560];snprintf(marker,sizeof(marker),"%s.offline",files[0].path);
 assert(access(marker,F_OK)==0);
 subsonic_client_t bad=*client;strcpy(bad.password,"wrong");
 assert(!subsonic_ping(&bad));
 assert(!subsonic_server_available(client->base_url,client->username));
 assert(subsonic_track_available(files[0].path));
 assert(subsonic_track_available(local));
 subsonic_song_t song={0};strcpy(song.id,"catalog-8");strcpy(song.suffix,"flac");
 char missing[512];assert(subsoniccache_path(client,&song,root,missing,sizeof(missing)));
 assert(!subsonic_track_available(missing));
 const char *queue[]={local,missing,files[0].path};
 playlist_load_paths_ordered(queue,3,0);
 char next[512];assert(playlist_next(next,sizeof(next)) && !strcmp(next,files[0].path));
 assert(playlist_prev(next,sizeof(next)) && !strcmp(next,local));
 playlist_load_paths_ordered(NULL,0,0);
 assert(subsonic_ping(client));
 assert(subsonic_track_available(missing));
 subsonic_client_t saved_account=account;account=*client;
 probe_at=0;lv_tick_inc(1100);offline_and_probe_tick();
 assert(!busy); // healthy server: no periodic ping
 subsonic_set_network_available(false);
 assert(!subsonic_network_available() && !subsonic_track_available(missing));
 assert(subsonic_track_available(files[0].path) && subsonic_track_available(local));
 lv_tick_inc(1100);offline_and_probe_tick();assert(!busy); // no offline retry
 subsonic_set_network_available(true);
 assert(!subsonic_track_available(missing)); // association alone is not server reachability
 assert(subsonic_ping(client) && subsonic_track_available(missing));
 account=saved_account;
 opt.max_bytes=files[0].bytes;
 assert(library_offline_save_options(source,&opt));
 assert(library_offline_rule(source,OFFLINE_SONG,"catalog-8",true));
 assert(subsonicoffline_start(client,root,false));
 for(int i=0;i<1000 && subsonicoffline_busy();++i)usleep(10000);
 assert(!subsonicoffline_busy() && access(missing,F_OK)!=0);
 assert(access(files[0].path,F_OK)==0 && access(local,F_OK)==0);
 opt.mode=OFFLINE_OFF;assert(library_offline_save_options(source,&opt));
}


static lv_obj_t *find_label(lv_obj_t *object,const char *text) {
 if(lv_obj_check_type(object,&lv_label_class) && !strcmp(lv_label_get_text(object),text))return object;
 for(uint32_t i=0;i<lv_obj_get_child_count(object);++i){lv_obj_t *found=find_label(lv_obj_get_child(object,i),text);if(found)return found;}
 return NULL;
}
static lv_obj_t *find_class(lv_obj_t *object,const lv_obj_class_t *type){
 if(lv_obj_check_type(object,type))return object;
 for(uint32_t i=0;i<lv_obj_get_child_count(object);++i){lv_obj_t *found=find_class(lv_obj_get_child(object,i),type);if(found)return found;}
 return NULL;
}
static int count_class(lv_obj_t *object,const lv_obj_class_t *type) {
 int n=lv_obj_check_type(object,type)?1:0;
 for(uint32_t i=0;i<lv_obj_get_child_count(object);++i)n+=count_class(lv_obj_get_child(object,i),type);
 return n;
}
static void test_navidrome_controls(subsonic_client_t *client,gui_config_t *cfg) {
 control_server(client->base_url,"test-recover-sync");
 assert(subsonic_ping(client));connected=true;rebuild_home();
 assert(!find_label(home,"Search songs") && !find_label(home,"Disconnect"));
 lv_obj_t *label=find_label(home,"Sync");assert(label && lv_obj_check_type(lv_obj_get_parent(label),&lv_button_class));
 assert(!find_label(home,"Remove connection"));
 assert(remove_connection_icon && !lv_obj_is_hidden(remove_connection_icon));
 confirm_init(cfg);forget_cb(NULL);assert(saved_connection && account.password[0]);confirm_close();
 bool saved=saved_connection;connected=false;saved_connection=false;clear_password();rebuild_home();
 label=find_label(home,"Password");assert(label);
 lv_obj_t *row=lv_obj_get_parent(label);assert(find_label(row,""));
 strcpy(password,"test-password");rebuild_home();
 assert(find_label(home,"••••••••") && !find_label(home,"test-password"));
 clear_password();saved_connection=saved;connected=true;rebuild_home();
 char source[33];subsoniccache_source(client,source);
 offline_options_t opt;assert(library_offline_options(source,&opt));
 opt.mode=OFFLINE_SELECTIVE;opt.automatic=0;opt.charging_only=0;opt.max_bytes=4096;opt.reserve_bytes=0;
 assert(library_offline_save_options(source,&opt));
 rebuild_home();assert(find_label(home,"Automatic sync") && !find_label(home,"Refresh interval"));
 assert(count_class(home,&lv_switch_class)==1);
 int home_height=lv_obj_get_height(home);lv_obj_set_height(home,180);lv_obj_update_layout(home);
 lv_obj_scroll_to_y(home,60,LV_ANIM_OFF);int home_y=lv_obj_get_scroll_y(home);assert(home_y>0);
 label=find_label(home,"Automatic sync");assert(label);
 lv_obj_t *automatic_row=lv_obj_get_parent(label);
 lv_obj_t *toggle=NULL;
 for(uint32_t i=0;i<lv_obj_get_child_count(automatic_row);++i){lv_obj_t *child=lv_obj_get_child(automatic_row,i);if(lv_obj_check_type(child,&lv_switch_class))toggle=child;}
 assert(toggle);lv_obj_add_state(toggle,LV_STATE_CHECKED);lv_obj_send_event(toggle,LV_EVENT_VALUE_CHANGED,NULL);
 assert(find_label(home,"Refresh interval") && lv_obj_get_scroll_y(home)==home_y);
 assert(library_offline_options(source,&opt) && opt.automatic);
 opt.automatic=0;assert(library_offline_save_options(source,&opt));rebuild_home();
 assert(!find_label(home,"Refresh interval"));
 lv_obj_set_height(home,home_height);
 subsonicofflinepage_open();
 lv_obj_t *offline=lv_screen_active();
 assert(count_class(offline,&lv_slider_class)==1 && count_class(offline,&lv_switch_class)==2);
 assert(find_label(offline,"Automatically cache recently played") && !find_label(offline,"Off") && !find_label(offline,"Reserve free space"));
 assert(!find_label(offline,"Choose how music should be kept offline.") && !find_label(offline,"Remove downloads no longer selected"));
 assert(find_label(offline,"Offline / cache settings"));
 label=find_label(offline,"Maximum offline storage");assert(label);
 lv_obj_t *storage=find_class(lv_obj_get_parent(label),&lv_slider_class);assert(storage);
 lv_slider_set_value(storage,8,LV_ANIM_OFF);lv_obj_send_event(storage,LV_EVENT_RELEASED,NULL);
 assert(find_label(offline,"Reserve free space") && count_class(offline,&lv_slider_class)==2);
 label=find_label(offline,"Maximum offline storage");storage=find_class(lv_obj_get_parent(label),&lv_slider_class);assert(storage);
 lv_slider_set_value(storage,3,LV_ANIM_OFF);lv_obj_send_event(storage,LV_EVENT_RELEASED,NULL);
 assert(!find_label(offline,"Reserve free space") && count_class(offline,&lv_slider_class)==1);

 assert(!find_label(offline,"Automatic sync") && !find_label(offline,"Refresh interval"));
 assert(!find_label(offline,"Choose songs") && !find_label(offline,"Choose albums") && !find_label(offline,"Choose server playlists"));
 label=find_label(offline,"Remove all downloads");assert(label && lv_obj_check_type(lv_obj_get_parent(label),&lv_button_class));
 lv_obj_t *offline_content=lv_obj_get_parent(lv_obj_get_parent(label));
 int old_height=lv_obj_get_height(offline_content);lv_obj_set_height(offline_content,180);lv_obj_update_layout(offline_content);
 lv_obj_scroll_to_y(offline_content,100,LV_ANIM_OFF);int offline_y=lv_obj_get_scroll_y(offline_content);assert(offline_y>0);
 label=find_label(offline,"Selective");assert(label);
 lv_obj_send_event(lv_obj_get_parent(label),LV_EVENT_CLICKED,NULL);
 assert(lv_obj_get_scroll_y(offline_content)==offline_y);lv_obj_set_height(offline_content,old_height);
 assert(!find_label(offline,"Sync offline music now"));
 sync_cb(NULL);assert(busy);pump_request();
 for(int i=0;i<1500 && subsonicoffline_busy();++i)usleep(10000);
 assert(!subsonicoffline_busy());
 assert(library_offline_options(source,&opt) && opt.last_run>0);
 assert(library_remote_count(source)==51);
 opt.mode=OFFLINE_OFF;opt.automatic=1;assert(library_offline_save_options(source,&opt));
 lv_tick_inc(1100);offline_retry_at=0;offline_and_probe_tick();assert(busy);pump_request();wait_scan();
 assert(library_offline_options(source,&opt) && opt.mode==OFFLINE_OFF && opt.last_run>0);
 assert(!subsonicoffline_busy()); // full automatic refresh works without offline downloads
 opt.automatic=0;assert(library_offline_save_options(source,&opt));
}


static void wait_offline(void){
 for(int i=0;i<1500 && subsonicoffline_busy();++i)usleep(10000);
 assert(!subsonicoffline_busy());
}
static lv_obj_t *find_glyph(lv_obj_t *object,const lv_image_dsc_t *glyph){
 if(lv_obj_is_hidden(object))return NULL;
 if(lv_obj_check_type(object,&lv_image_class) && lv_image_get_src(object)==glyph)return object;
 for(uint32_t i=0;i<lv_obj_get_child_count(object);++i){lv_obj_t *found=find_glyph(lv_obj_get_child(object,i),glyph);if(found)return found;}
 return NULL;
}
static int dimmed_buttons(lv_obj_t *object){
 if(lv_obj_is_hidden(object))return 0;
 lv_obj_t *label=lv_obj_check_type(object,&lv_button_class)?find_class(object,&lv_label_class):NULL;
 int count=label && lv_obj_get_style_text_opa(label,0)==LV_OPA_40;
 if(count)assert(lv_obj_get_style_opa(object,0)==LV_OPA_COVER); // gray content, opaque scrolling cards
 for(uint32_t i=0;i<lv_obj_get_child_count(object);++i)count+=dimmed_buttons(lv_obj_get_child(object,i));
 return count;
}
static void refresh_gui(void){gui_post_drain();lv_tick_inc(1100);lv_timer_handler();gui_post_drain();}
static void test_context_downloads(subsonic_client_t *client,const char *root,const char *local,gui_config_t *cfg){
 char source[33];subsoniccache_source(client,source);offline_options_t options;
 assert(library_offline_options(source,&options));options.mode=OFFLINE_SELECTIVE;options.automatic=0;
 options.max_bytes=65536;options.reserve_bytes=0;assert(library_offline_save_options(source,&options));
 subsonic_song_t song={0};strcpy(song.id,"catalog-9");strcpy(song.suffix,"flac");
 char path[512];assert(subsoniccache_path(client,&song,root,path,sizeof(path)));const char *paths[]={path,local};
 assert(subsonicpage_download_state(paths,2)==0);
 medialist_screen=lv_obj_create(NULL);medialist_tracks_screen=lv_obj_create(NULL);medialist_albums_screen=lv_obj_create(NULL);
 medialist_init(cfg);popover_init();topbar_init(cfg);
 char album[256];assert(library_track_album_value(path,album,sizeof(album)));
 medialist_open("Catalog album",LIBRARY_LIST_TRACKS,LIBRARY_FILTER_ALBUM,album);
 assert(find_glyph(medialist_tracks_screen,&icon_download));
 assert(lv_obj_get_index(lv_obj_get_parent(find_glyph(medialist_tracks_screen,&icon_download)))==0);
 medialist_set_album_head(true);
 assert(find_glyph(medialist_tracks_screen,&icon_download)); // alternative cover-and-play header
 assert(lv_obj_get_index(lv_obj_get_parent(find_glyph(medialist_tracks_screen,&icon_download)))==0);
 medialist_set_album_head(false);
 assert(library_playlist_create("Download controls"));
 library_playlist_row_t row={0};snprintf(row.path,sizeof(row.path),"%s",path);row.present=true;
 assert(library_playlist_append("Download controls",&row));
 medialist_open("Download controls",LIBRARY_LIST_PLAYLIST,LIBRARY_FILTER_NONE,"Download controls");
 assert(find_glyph(medialist_tracks_screen,&icon_download));
 lv_obj_t *ellipsis=find_glyph(medialist_tracks_screen,&icon_ellipsis_vertical);assert(ellipsis);
 lv_obj_send_event(lv_obj_get_parent(ellipsis),LV_EVENT_CLICKED,NULL);
 assert(find_label(lv_layer_top(),"Download"));popover_close();
 snprintf(test_current_path,sizeof(test_current_path),"%s",path);
 trackmenu_open(lv_obj_get_parent(ellipsis));assert(find_label(lv_layer_top(),"Download"));popover_close();
 test_current_path[0]=0;

 control_server(client->base_url,"test-slow-download");
 assert(subsonicpage_download_paths(paths,2,OFFLINE_SONG,NULL,false));
 assert(subsonicpage_sync_busy());
 medialist_open("Download controls",LIBRARY_LIST_PLAYLIST,LIBRARY_FILTER_NONE,"Download controls");
 assert(find_glyph(medialist_tracks_screen,&icon_refresh));
 refresh_gui();
 assert(find_glyph(medialist_tracks_screen,&icon_refresh));
 assert(find_label(home,"Cancel sync") && !find_label(home,"Sync"));
 assert(!find_label(home,"Stop offline sync") && !find_label(home,"Cancel download"));
 assert(find_glyph(lv_layer_top(),&icon_refresh));
 wait_offline();gui_post_drain();
 assert(find_glyph(medialist_tracks_screen,&icon_trash));
 topbar_refresh_navidrome();
 assert(!subsonicpage_sync_busy() && !find_glyph(lv_layer_top(),&icon_refresh));
 rebuild_home();assert(find_label(home,"Sync") && !find_label(home,"Cancel sync"));


 assert(subsonicpage_download_state(paths,2)==1 && access(local,F_OK)==0);
 medialist_open("Download controls",LIBRARY_LIST_PLAYLIST,LIBRARY_FILTER_NONE,"Download controls");
 assert(find_glyph(medialist_tracks_screen,&icon_trash));
 ellipsis=find_glyph(medialist_tracks_screen,&icon_ellipsis_vertical);assert(ellipsis);
 lv_obj_send_event(lv_obj_get_parent(ellipsis),LV_EVENT_CLICKED,NULL);
 assert(find_label(lv_layer_top(),"Remove download"));popover_close();
 snprintf(test_current_path,sizeof(test_current_path),"%s",path);
 trackmenu_open(lv_obj_get_parent(ellipsis));assert(find_label(lv_layer_top(),"Remove download"));popover_close();
 test_current_path[0]=0;

 subsonic_identity_t identity;assert(subsoniccache_identity(path,&identity));
 // Another connection's download is not part of Remove all downloads.
 subsonic_client_t other=*client;strcpy(other.username,"another-user");
 char other_path[512];assert(subsoniccache_path(&other,&song,root,other_path,sizeof(other_path)));
 FILE *file=fopen(other_path,"wb");assert(file);fputs("foreign cache",file);fclose(file);
 strcpy(identity.username,other.username);
 char sidecar[560];snprintf(sidecar,sizeof(sidecar),"%s.identity",other_path);
 file=fopen(sidecar,"wb");assert(file);assert(fwrite(&identity,sizeof(identity),1,file)==1);fclose(file);

 // One fixed-position button cancels offline work; completed downloads survive.
 subsonic_song_t cancel_song={0};strcpy(cancel_song.id,"catalog-12");strcpy(cancel_song.suffix,"flac");
 char cancel_path[512];assert(subsoniccache_path(client,&cancel_song,root,cancel_path,sizeof(cancel_path)));
 const char *cancel_paths[]={cancel_path};
 rebuild_home();uint32_t controls=lv_obj_get_child_count(home);
 streamturn_acquire(); // hold the worker before its transfer so cancellation is deterministic
 assert(subsonicpage_download_paths(cancel_paths,1,OFFLINE_SONG,NULL,false));
 rebuild_home();assert(lv_obj_get_child_count(home)==controls);
 lv_obj_t *cancel_label=find_label(home,"Cancel sync");assert(cancel_label);
 lv_obj_send_event(lv_obj_get_parent(cancel_label),LV_EVENT_CLICKED,NULL);
 assert(atomic_load(&play_cancelled));
 streamturn_release();wait_offline();rebuild_home();
 assert(find_label(home,"Sync") && !find_label(home,"Cancel sync"));
 assert(access(cancel_path,F_OK)!=0 && access(path,F_OK)==0);
 // Removing a track from a selected album creates an exclusion.
 char ids[1][128]={"catalog-9"};
 assert(library_offline_select(source,OFFLINE_ALBUM,"catalog-album",ids,1,true));
 assert(subsonicpage_download_paths(paths,1,OFFLINE_SONG,NULL,true));wait_offline();
 assert(access(path,F_OK)!=0 && access(local,F_OK)==0);
 assert(library_offline_has_rule(source,OFFLINE_EXCLUDE,"catalog-9"));
 assert(library_offline_plan(source,false,NULL,0));
 offline_file_t files[60];int n=library_offline_files(source,0,files,60);assert(n==50);
 for(int i=0;i<n;++i)assert(strcmp(files[i].id,"catalog-9") || !files[i].wanted);
 // Selecting the album again clears that exclusion.
 assert(library_offline_select(source,OFFLINE_ALBUM,"catalog-album",ids,1,true));
 assert(!library_offline_has_rule(source,OFFLINE_EXCLUDE,"catalog-9"));
 assert(library_offline_clear(source));
 options.mode=OFFLINE_SELECTIVE;assert(library_offline_save_options(source,&options));
 assert(subsonicpage_download_paths(paths,1,OFFLINE_SONG,NULL,false));wait_offline();
 assert(access(path,F_OK)==0);
 int tracks=library_track_count();
 remove_all_confirmed(NULL);wait_offline();
 assert(access(path,F_OK)!=0 && access(local,F_OK)==0 && access(other_path,F_OK)==0);
 assert(library_track_count()==tracks && library_remote_count(source)==51);
 assert(library_offline_options(source,&options) && options.mode==OFFLINE_SELECTIVE && !options.auto_cache);
 assert(!library_offline_has_rule(source,OFFLINE_SONG,"catalog-9"));
 refresh_gui();assert(find_glyph(medialist_tracks_screen,&icon_download) && !find_glyph(medialist_tracks_screen,&icon_trash));

}


static void test_temporary_playback(subsonic_client_t *client,const char *root,const char *local,const char *database){
 char source[33];subsoniccache_source(client,source);offline_options_t options;
 assert(library_offline_options(source,&options));options.mode=OFFLINE_SELECTIVE;options.auto_cache=0;options.reserve_bytes=0;
 assert(library_offline_save_options(source,&options));
 library_close();assert(library_open(database));
 assert(library_offline_options(source,&options) && !options.auto_cache); // preference persists
 subsonic_song_t song={0};strcpy(song.id,"catalog-10");strcpy(song.suffix,"flac");
 char path[512];assert(subsoniccache_path(client,&song,root,path,sizeof(path)));const char *paths[]={path};
 playlist_load_paths_ordered(paths,1,0);
 snprintf(test_current_path,sizeof(test_current_path),"%s",path);test_audio_status=AUDIO_STATUS_PLAYING;
 assert(!prepare_track(path));pump_request();
 assert(access(path,F_OK)==0 && subsoniccache_is_temporary(path));
 assert(subsonicpage_download_state(paths,1)==0); // playback spool is not an offline download
 discard_finished_temporary();assert(access(path,F_OK)==0);
 test_audio_status=AUDIO_STATUS_PAUSED;discard_finished_temporary();assert(access(path,F_OK)==0);
 test_audio_status=AUDIO_STATUS_STOPPED;discard_finished_temporary();assert(access(path,F_OK)!=0);
 test_audio_status=AUDIO_STATUS_PLAYING;assert(!prepare_track(path));pump_request();
 assert(subsoniccache_is_temporary(path));
 assert(subsonicpage_download_paths(paths,1,OFFLINE_SONG,NULL,false));wait_offline();
 assert(!subsoniccache_is_temporary(path) && subsonicpage_download_state(paths,1)==1);
 test_audio_status=AUDIO_STATUS_STOPPED;discard_finished_temporary();assert(access(path,F_OK)==0); // explicit download retained
 test_current_path[0]=0;
 strcpy(song.id,"catalog-11");char stale[512];assert(subsoniccache_fetch(client,&song,root,stale,sizeof(stale)));
 assert(subsoniccache_mark_temporary(stale));
 subsoniccache_cleanup_temporary(client,root,stale);assert(access(stale,F_OK)==0); // keep active track
 subsoniccache_cleanup_temporary(client,root,NULL);
 assert(access(stale,F_OK)!=0 && access(path,F_OK)==0 && access(local,F_OK)==0);
}


static void test_removed_server_files(subsonic_client_t *client,const char *root,const char *local){
 atomic_bool cancel=false;char source[33];subsoniccache_source(client,source);
 subsonic_song_t song={0};strcpy(song.suffix,"flac");strcpy(song.id,"catalog-13");
 char removed[512],active[512];assert(subsoniccache_fetch(client,&song,root,removed,sizeof(removed)));
 offline_options_t options;assert(library_offline_options(source,&options));options.mode=OFFLINE_SELECTIVE;options.automatic=0;options.reserve_bytes=0;options.max_bytes=65536;
 assert(library_offline_save_options(source,&options));char ids[1][128]={"catalog-13"};
 assert(library_offline_select(source,OFFLINE_SONG,NULL,ids,1,true));
 assert(subsonicoffline_start(client,root,false));wait_offline();
 char side[560];snprintf(side,sizeof(side),"%s.offline",removed);assert(access(side,F_OK)==0);
 strcpy(song.id,"catalog-14");assert(subsoniccache_fetch(client,&song,root,active,sizeof(active)));
 control_server(client->base_url,"test-remove-track-13");
 control_server(client->base_url,"test-fail-next-sync");
 assert(subsoniccatalog_sync(client,root,&cancel)==-1 && access(removed,F_OK)==0);
 library_remote_track_t track;assert(library_remote_get(removed,&track));
 atomic_store(&cancel,true);assert(subsoniccatalog_sync(client,root,&cancel)==-1 && access(removed,F_OK)==0);atomic_store(&cancel,false);

 control_server(client->base_url,"test-recover-sync");
 // Cleanup must reject a mismatched owner and keep a durable retry record.
 subsonic_identity_t original,wrong;assert(subsoniccache_identity(removed,&original));wrong=original;strcpy(wrong.username,"different-account");
 snprintf(side,sizeof(side),"%s.identity",removed);FILE *identity_file=fopen(side,"wb");assert(identity_file);assert(fwrite(&wrong,sizeof(wrong),1,identity_file)==1);fclose(identity_file);
 assert(subsoniccatalog_sync(client,root,&cancel)==-1 && access(removed,F_OK)==0);
 assert(!library_remote_get(removed,&track) && library_remote_removed_pending(source,NULL));
 char database[600];snprintf(database,sizeof(database),"%s/library.db",root);library_close();assert(library_open(database));
 assert(library_remote_removed_pending(source,NULL));
 identity_file=fopen(side,"wb");assert(identity_file);assert(fwrite(&original,sizeof(original),1,identity_file)==1);fclose(identity_file);
 assert(subsoniccatalog_sync(client,root,&cancel)==50);
 assert(access(removed,F_OK)!=0 && !library_remote_get(removed,&track));
 const char *suffixes[]={".offline",".tags",".identity",".temporary"};
 for(int i=0;i<4;++i){snprintf(side,sizeof(side),"%s%s",removed,suffixes[i]);assert(access(side,F_OK)!=0);}
 assert(access(active,F_OK)==0 && access(local,F_OK)==0);
 assert(!subsonic_track_available(removed)); // stale queue entries must not attempt missing files
 // Active playback survives catalog removal, then cleanup runs after stopping.
 snprintf(test_current_path,sizeof(test_current_path),"%s",active);test_audio_status=AUDIO_STATUS_PAUSED;
 control_server(client->base_url,"test-remove-track-14");
 assert(subsoniccatalog_sync(client,root,&cancel)==49 && access(active,F_OK)==0);
 assert(library_remote_removed_pending(source,NULL) && !library_remote_removed_pending(source,active));
 test_audio_status=AUDIO_STATUS_STOPPED;cleanup_retry_at=0;
 start_pending_cleanup();assert(busy);pump_request();
 assert(access(active,F_OK)!=0 && !library_remote_removed_pending(source,NULL));
 test_current_path[0]=0;
 assert(access(local,F_OK)==0);
 control_server(client->base_url,"test-restore-tracks");
 assert(subsoniccatalog_sync(client,root,&cancel)==51);
}

static void test_unchanged_offline_sync(subsonic_client_t *client,const char *root){
 char source[33];subsoniccache_source(client,source);offline_options_t options;
 assert(library_offline_options(source,&options));options.mode=OFFLINE_MIRROR;options.automatic=0;options.charging_only=0;options.max_bytes=131072;options.reserve_bytes=0;
 assert(library_offline_save_options(source,&options));
 assert(subsonicoffline_start(client,root,false));wait_offline();
 subsonic_offline_status_t status;subsonicoffline_status(&status);assert(status.done==51 && status.total==51);
 subsonic_song_t song={0};strcpy(song.id,"catalog-0");strcpy(song.suffix,"flac");
 char path[512],marker[560];assert(subsoniccache_path(client,&song,root,path,sizeof(path)));snprintf(marker,sizeof(marker),"%s.offline",path);
 struct stat before,after;assert(stat(marker,&before)==0);
 atomic_store(&measured_fetches,0);atomic_store(&measured_notes,0);atomic_store(&measured_sums,0);atomic_store(&measure_unchanged,true);
 struct timespec began,ended;clock_gettime(CLOCK_MONOTONIC,&began);
 assert(subsonicoffline_start(client,root,false));wait_offline();clock_gettime(CLOCK_MONOTONIC,&ended);
 atomic_store(&measure_unchanged,false);
 long long elapsed=(long long)(ended.tv_sec-began.tv_sec)*1000+(ended.tv_nsec-began.tv_nsec)/1000000;
 assert(atomic_load(&measured_fetches)==0 && atomic_load(&measured_notes)==0 && atomic_load(&measured_sums)==1);
 assert(stat(marker,&after)==0 && before.st_mtim.tv_sec==after.st_mtim.tv_sec && before.st_mtim.tv_nsec==after.st_mtim.tv_nsec);
 subsonicoffline_status(&status);assert(status.done==51 && status.total==51);
 fprintf(stderr,"Unchanged offline sync: 51 cached tracks, %lld ms, zero fetches or per-track DB writes.\n",elapsed);
 assert(elapsed<2000); // the old unconditional delay alone took 5100 ms
 options.mode=OFFLINE_SELECTIVE;assert(library_offline_save_options(source,&options));
}
static int ordinary_events,coalesced_events;
static void ordinary_event(void *unused){(void)unused;++ordinary_events;}
static void coalesced_event(void *unused){(void)unused;++coalesced_events;}
static void test_state_mailbox(void){
 gui_post_drain();
 int queued=0;while(queued<1024 && gui_post(ordinary_event,NULL))++queued;
 assert(queued>0 && queued<1024);
 for(int i=0;i<1000;++i)assert(gui_post_coalesced(coalesced_event,NULL));
 gui_post_drain();
 assert(ordinary_events==queued && coalesced_events==1);
}

static void test_deleted_slider_theme(void){
 int original=theme_get_accent();bool tint=theme_dynamic_tint();
 for(int i=0;i<24;++i){
  lv_obj_t *page=lv_obj_create(NULL),*slider;
  settingsrow_slider(page,"Temporary slider",7,NULL,&slider,NULL);
  lv_obj_delete(page); // a rebuilt settings page must leave no stale theme target
  theme_set_accent((original+i+1)%THEME_ACCENT_COUNT);
  theme_set_dynamic_tint(!theme_dynamic_tint());
 }
 theme_set_accent(original);theme_set_dynamic_tint(tint);
 // Theme repainting must also cover more live sliders than the former 16-slot cap.
 bool dark=theme_is_dark();lv_obj_t *page=lv_obj_create(NULL),*sliders[32];
 for(int i=0;i<32;++i)settingsrow_slider(page,"Live slider",7,NULL,&sliders[i],NULL);
 theme_set_dark(!dark);
 for(int i=0;i<32;++i)assert(lv_obj_get_style_border_width(sliders[i],LV_PART_KNOB)==(theme_is_dark()?1:2));
 lv_obj_delete(page);theme_set_dark(dark);
}


static void cleanup_write(const char *path,const void *bytes,size_t size) {
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(bytes,1,size,f)==size);assert(fclose(f)==0);
}
static void test_connection_cleanup(subsonic_client_t *client,const char *root,const char *local,const char *database,const char *settings,const library_remote_track_t *reference) {
    char source[33];subsoniccache_source(client,source);
    char art[640];assert(subsonicart_for_track(reference->path,art,sizeof(art)));
    subsonic_identity_t identity;assert(subsoniccache_identity(reference->path,&identity));
    const char *extras[]={".part",".tags.tmp",".identity.tmp",".offline",".temporary"};
    for(unsigned i=0;i<sizeof(extras)/sizeof(extras[0]);i++) {
        char side[600];snprintf(side,sizeof(side),"%s%s",reference->path,extras[i]);cleanup_write(side,"extra",5);
    }
    char lyric[600],lyric_part[620];
    const char *extension=strrchr(reference->path,'.');assert(extension);
    snprintf(lyric,sizeof(lyric),"%.*s.lrc",(int)(extension-reference->path),reference->path);
    snprintf(lyric_part,sizeof(lyric_part),"%s.part",lyric);
    cleanup_write(lyric,"words",5);cleanup_write(lyric_part,"partial",7);
    char obsolete_art[640];snprintf(obsolete_art,sizeof(obsolete_art),"%s/.local/subsonic-art/%s-obsolete.jpg",root,source);
    cleanup_write(obsolete_art,"old",3);
    const char *paths[]={reference->path};
    assert(subsonicoffline_remove(client,root,paths,1,false));wait_offline();
    assert(access(reference->path,F_OK)!=0 && access(art,F_OK)==0 && access(obsolete_art,F_OK)!=0); // keep shared catalog art, remove orphan art
    assert(access(lyric,F_OK)!=0 && access(lyric_part,F_OK)!=0);
    const char *all_side[]={".part",".tags.tmp",".identity.tmp",".offline",".temporary",".tags",".identity"};
    for(unsigned i=0;i<sizeof(all_side)/sizeof(all_side[0]);i++) {
        char side[600];snprintf(side,sizeof(side),"%s%s",reference->path,all_side[i]);assert(access(side,F_OK)!=0);
    }
    char restored[512];assert(subsoniccache_fetch(client,&reference->song,root,restored,sizeof(restored)));
    // An owned orphan outside the catalog and an abandoned art write.
    subsonic_song_t orphan=reference->song;strcpy(orphan.id,"cleanup-orphan");
    char orphan_path[512];assert(subsoniccache_path(client,&orphan,root,orphan_path,sizeof(orphan_path)));
    cleanup_write(orphan_path,"owned",5);
    subsonic_identity_t orphan_identity=identity;strcpy(orphan_identity.song_id,orphan.id);
    char side[600];snprintf(side,sizeof(side),"%s.identity",orphan_path);cleanup_write(side,&orphan_identity,sizeof(orphan_identity));
    char orphan_art[640];snprintf(orphan_art,sizeof(orphan_art),"%s/.local/subsonic-art/%s-orphan.jpg.tmp",root,source);cleanup_write(orphan_art,"art",3);

    // A second account's media/art must survive.
    subsonic_client_t other=*client;strcpy(other.username,"cleanup-other");
    library_remote_track_t foreign=*reference;strcpy(foreign.song.id,"foreign-cleanup");
    subsoniccache_source(&other,foreign.source);strcpy(foreign.username,other.username);
    assert(subsoniccache_path(&other,&foreign.song,root,foreign.path,sizeof(foreign.path)));
    assert(library_remote_begin(foreign.source) && library_remote_stage(&foreign,1) && library_remote_publish(foreign.source));
    cleanup_write(foreign.path,"foreign",7);
    subsonic_identity_t foreign_identity=identity;strcpy(foreign_identity.username,other.username);strcpy(foreign_identity.song_id,foreign.song.id);
    snprintf(side,sizeof(side),"%s.identity",foreign.path);cleanup_write(side,&foreign_identity,sizeof(foreign_identity));
    char foreign_art[640];snprintf(foreign_art,sizeof(foreign_art),"%s/.local/subsonic-art/%s-keep.jpg",root,foreign.source);cleanup_write(foreign_art,"keep",4);

    assert(playlists_create("Cleanup server"));
    assert(playlists_add_track("Cleanup server",reference->path));
    char backup[640];assert(playlists_backup("Cleanup server",backup,sizeof(backup)));
    assert(playlists_create("Cleanup local"));
    assert(playlists_add_track("Cleanup local",local));
    assert(playlists_add_track("Cleanup local",reference->path));
    sqlite3 *fixture=NULL;assert(sqlite3_open(database,&fixture)==SQLITE_OK);
    char *sql=sqlite3_mprintf("INSERT INTO REMOTE_PLAYLISTS VALUES(%Q,'cleanup-playlist','Cleanup server','Cleanup server','Server playlist','');",source);
    assert(sql && sqlite3_exec(fixture,sql,NULL,NULL,NULL)==SQLITE_OK);sqlite3_free(sql);sqlite3_close(fixture);

    // Ownership mismatch fails safely and leaves the saved login/catalog retryable.
    subsonic_identity_t wrong=identity;strcpy(wrong.username,"not-this-account");
    snprintf(side,sizeof(side),"%s.identity",reference->path);cleanup_write(side,&wrong,sizeof(wrong));
    assert(!subsoniccleanup_connection(client,root));
    assert(library_remote_count(source)==2);
    assert(access(reference->path,F_OK)==0 && access(settings,F_OK)==0);
    cleanup_write(side,&identity,sizeof(identity));

    if(busy)pump_request();
    account=*client;connected=saved_connection=true;assert(subsonicaccount_save(settings,client));
    assert(!busy && !subsonicoffline_busy());
    forget_confirmed(NULL);assert(busy);pump_request();
    assert(!saved_connection && !account.base_url[0] && library_remote_count(source)==0);
    assert(library_remote_count(foreign.source)==1 && library_track_count()==2);
    assert(access(local,F_OK)==0 && access(foreign.path,F_OK)==0 && access(foreign_art,F_OK)==0);
    assert(access(reference->path,F_OK)!=0 && access(orphan_path,F_OK)!=0 && access(art,F_OK)!=0 && access(orphan_art,F_OK)!=0);
    assert(!playlists_exists("Cleanup server") && access(backup,F_OK)!=0);
    assert(playlists_exists("Cleanup local") && library_playlist_count("Cleanup local")==1);
    library_playlist_link_t *links=NULL;assert(library_playlist_links(source,&links)==0);free(links);
    subsonic_client_t saved;assert(!subsonicaccount_load(settings,&saved));
    library_close();assert(library_open(database));
    assert(library_remote_count(source)==0 && library_remote_count(foreign.source)==1 && library_track_count()==2);
    assert_albums_populated();
    puts("Complete connection cleanup and download sidecar tests passed");
}
int main(int argc, char **argv) {
	assert(argc == 3);
	char settings[512], database[512], account_path[600];
	snprintf(settings, sizeof(settings), "%s/settings.ini", argv[1]);
	snprintf(database, sizeof(database), "%s/library.db", argv[1]);
	snprintf(account_path, sizeof(account_path), "%s.subsonic-account", settings);
	assert(config_init(settings));
	subsonic_client_t client, restored;
	assert(subsonic_client_init(&client, argv[2], "user", "private-password"));
	assert(subsonicaccount_save(settings, &client));
	struct stat st;
	assert(!stat(account_path, &st) && (st.st_mode & 0777) == 0600);
	assert(subsonicaccount_load(settings, &restored));
	assert(!strcmp(client.base_url, restored.base_url));
	assert(!strcmp(client.password, restored.password));
	assert(chmod(account_path, 0644) == 0);
	assert(!subsonicaccount_load(settings, &restored)); // refuse exposed credentials
	assert(subsonicaccount_save(settings, &client));
	assert(subsonicaccount_load(settings, &restored));
	subsonic_client_clear(&restored);

	assert(library_open(database));
	assert(library_scan_start(argv[1])); wait_scan();
	assert(library_track_count() == 1); // the fixture's local WAV
	library_remote_track_t remote = {0}, readback;
	strcpy(remote.server, client.base_url); strcpy(remote.username, client.username);
	subsoniccache_source(&client, remote.source);
	strcpy(remote.song.id, "remote-song"); strcpy(remote.song.album_id, "remote-album");
	strcpy(remote.song.title, "Remote title"); strcpy(remote.song.artist, "Remote artist");
	strcpy(remote.song.album, "Remote album"); strcpy(remote.song.suffix, "flac");
	assert(subsoniccache_path(&client, &remote.song, argv[1], remote.path, sizeof(remote.path)));
	assert(library_remote_begin(remote.source));
	assert(library_remote_stage(&remote, 1));
	assert(library_track_count() == 1); // staging is invisible
	assert(library_remote_publish(remote.source));
	assert(library_track_count() == 2);
	assert(library_remote_count(remote.source) == 1);
	assert(library_remote_get(remote.path, &readback));
	assert(!strcmp(readback.song.id, "remote-song"));
	assert(!strcmp(readback.server, client.base_url));
	assert(!strcmp(readback.username, client.username));
	assert(access(remote.path, F_OK) != 0); // metadata-only, no placeholder/audio
	char names[10][256];
	assert(library_list(LIBRARY_LIST_ARTISTS, names, 10) >= 1);
	assert(library_list(LIBRARY_LIST_ALBUMS, names, 10) >= 1);
	int albums_before_restart = album_group_count();
	assert(library_reorganize()); wait_scan();
	assert(album_group_count() == albums_before_restart);
	assert_albums_populated();
	// Recreate a database written by the buggy version: organizer changed media
	// keys, while the original remote group was later reinserted at startup.
	library_close();
	sqlite3 *legacy = NULL;
	assert(sqlite3_open(database, &legacy) == SQLITE_OK);
	assert(sqlite3_exec(legacy,
		"UPDATE REMOTE_MEDIA SET album=album||' ';"
        "UPDATE MEDIA_TABLE SET album_key='aold' WHERE path IN(SELECT path FROM REMOTE_MEDIA);"
        "INSERT OR IGNORE INTO ALBUM_GROUP_TABLE(album,album_key,sortkey) SELECT album,album_key,album FROM MEDIA_TABLE WHERE path IN(SELECT path FROM REMOTE_MEDIA);"
        "INSERT OR IGNORE INTO ALBUM_GROUP_TABLE(album,album_key,sortkey) SELECT album,album_key,album FROM REMOTE_MEDIA;"
		"INSERT OR IGNORE INTO ALBUM_GROUP_TABLE(album,album_key,sortkey) SELECT album,'aold',album FROM REMOTE_MEDIA;"
        "INSERT INTO ALBUM_GROUP_TABLE(album,album_key,sortkey) VALUES('Removed album one',NULL,'removed1'),('Removed album two',NULL,'removed2'),('Removed album three','orphan-key','removed3');"
        "UPDATE ALBUM_GROUP_TABLE SET album_key=NULL WHERE album IN(SELECT album FROM MEDIA_TABLE WHERE path NOT IN(SELECT path FROM REMOTE_MEDIA));",
		NULL, NULL, NULL) == SQLITE_OK);
	sqlite3_close(legacy);
	assert(library_open(database));
	assert(album_group_count() == albums_before_restart);
	assert_albums_populated();
	library_close();
	assert(library_open(database));
	assert(album_group_count() == albums_before_restart);
	// An interrupted replacement leaves the old, playable catalog visible.
	assert(library_remote_begin(remote.source));
	strcpy(remote.song.title, "Unpublished title");
	assert(library_remote_stage(&remote, 1));
	assert(library_remote_get(remote.path, &readback));
	assert(!strcmp(readback.song.title, "Remote title"));
	library_close();
	assert(library_open(database));
	assert(library_track_count() == 2);
	assert(library_remote_get(remote.path, &readback));
	// A full card scan must not remove virtual remote entries.
	assert(library_scan_start(argv[1])); wait_scan();
	assert(library_track_count() == 2);
	assert(library_remote_get(remote.path, &readback));
	// Detect changes must also preserve the remote catalog.
	library_set_detect_changes(true);
	library_card_returned(argv[1]); wait_scan();
	assert(library_track_count() == 2);
	assert(library_remote_get(remote.path, &readback));
	assert(library_remote_begin(remote.source)); // publish empty catalog for just this source
	assert(library_remote_publish(remote.source));
	assert(library_track_count() == 1);
	assert(!library_remote_get(remote.path, &readback));
	atomic_bool cancelled = false;
	assert(subsoniccatalog_sync(&client, argv[1], &cancelled) == 51);
	assert(strstr(client.error, "1 cover(s) unavailable")); // missing art is nonfatal
	assert(library_track_count() == 52);
	assert(library_remote_count(remote.source) == 51);
	strcpy(remote.song.id, "catalog-0");
	assert(subsoniccache_path(&client, &remote.song, argv[1], remote.path, sizeof(remote.path)));
	assert(library_remote_get(remote.path, &readback));
	assert(!strcmp(readback.song.title, "Catalog title 0"));
	assert(!strcmp(readback.song.album_artist, "Catalog album artist"));
	assert(!strcmp(readback.song.genre, "Rock"));
	assert(readback.song.year == 2020 && readback.song.disc == 2);

 assert(access(remote.path,F_OK)!=0);
 song_metadata_t pending_metadata;metadata_read(remote.path,&pending_metadata);
 assert(library_remote_metadata(remote.path,&pending_metadata));
 assert(!strcmp(pending_metadata.title,"Catalog title 0") && pending_metadata.has_tags);
 assert(!strcmp(pending_metadata.artist,"Catalog artist") && !strcmp(pending_metadata.album,"Catalog album"));
 assert(pending_metadata.track_number==1 && pending_metadata.disc_number==2 && pending_metadata.year==2020);
 subsonic_set_network_available(false);
 memset(&pending_metadata,0,sizeof(pending_metadata));
 assert(library_remote_metadata(remote.path,&pending_metadata) && !strcmp(pending_metadata.title,"Catalog title 0"));
 subsonic_set_network_available(true);assert(subsonic_ping(&client));
 strcpy(pending_metadata.title,"Embedded title");pending_metadata.has_track_gain=true;pending_metadata.track_gain_db=-4.5f;
 assert(library_remote_metadata(remote.path,&pending_metadata));
 assert(!strcmp(pending_metadata.title,"Embedded title") && pending_metadata.has_track_gain && pending_metadata.track_gain_db==-4.5f);
 assert(!library_remote_metadata("/local/music.wav",&pending_metadata));
 lv_init();
	assert(lv_display_create(480, 720));
	assert(fonts_init()); theme_init();test_state_mailbox();test_deleted_slider_theme();
	char art_path[640];
	assert(subsonicart_for_track(remote.path, art_path, sizeof(art_path)));
	albumart_t art;
	assert(albumart_load_for_file(remote.path, &art)); // no audio has been downloaded
	assert(art.size > 0); albumart_free(&art);
	cover_set_cache_dir(argv[1]);
	cover_image_t thumb;
	assert(cover_thumb_load(remote.path, false, 96, &thumb));
	cover_free(&thumb);
	subsonic_song_t other_song = remote.song;
	strcpy(other_song.id, "catalog-1");
	char other_path[512], other_art[640];
	assert(subsoniccache_path(&client, &other_song, argv[1], other_path, sizeof(other_path)));
	assert(subsonicart_for_track(other_path, other_art, sizeof(other_art)));
	assert(!strcmp(art_path, other_art)); // shared across tracks, one download
	strcpy(other_song.id, "catalog-50");
	assert(subsoniccache_path(&client, &other_song, argv[1], other_path, sizeof(other_path)));
	assert(!subsonicart_for_track(other_path, other_art, sizeof(other_art)));
	assert(!cover_thumb_load(other_path, false, 96, &thumb)); // cache negative result
	char control[640], *body;
	snprintf(control, sizeof(control), "%s/test-recover-art", argv[2]);
	assert(http_get(control, &body, NULL, 1024, 5)); free(body);
	assert(subsoniccatalog_sync(&client, argv[1], &cancelled) == 51);
	assert(!client.error[0]);
	assert(access(other_path, F_OK) != 0);
	assert(cover_thumb_load(other_path, false, 96, &thumb)); // artwork invalidates negative thumb
	cover_free(&thumb);
	snprintf(control, sizeof(control), "%s/test-fail-next-sync", argv[2]);
	assert(http_get(control, &body, NULL, 1024, 5)); free(body);
	assert(subsoniccatalog_sync(&client, argv[1], &cancelled) == -1);
	assert(library_track_count() == 52);
	assert(library_remote_get(remote.path, &readback));
	assert(!strcmp(readback.song.title, "Catalog title 0"));
	atomic_store(&cancelled, true);
	assert(subsoniccatalog_sync(&client, argv[1], &cancelled) == -1);
	assert(library_track_count() == 52);
	assert(library_scan_start(argv[1])); wait_scan();
	assert(library_track_count() == 52);
	library_close();
	assert(library_open(database));
	assert(library_track_count() == 52);
	assert(subsonicart_for_track(remote.path, art_path, sizeof(art_path)));
	assert(albumart_load_for_file(remote.path, &art)); albumart_free(&art);
	assert(access(remote.path, F_OK) != 0); // artwork survives scans/restarts without audio
	// Initialize the real Navidrome UI without a window or audio output. Wrap
	// only the final playback handoff, so preparation and queue identity are real.
	gui_config_t cfg = {.screen_width=480,.screen_height=720,.padding=16,.top_bar_height=32,.sd_root_path=argv[1]};
	subsonicpage_init(&cfg);
	pump_request();
	assert(connected); // saved account reauthenticated without password entry
	char local[512]; snprintf(local, sizeof(local), "%s/Local.wav", argv[1]);
	const char *queue[] = {remote.path, local};
	playlist_load_paths(queue, 2, 0);
	assert(!prepare_track(remote.path));
	pump_request();
	assert(resumed_index == 0);
	assert(playlist_count() == 2 && playlist_current_index() == 0);
	assert(access(remote.path, F_OK) == 0);
	assert(prepare_track(remote.path)); // downloaded tracks play offline
	assert(prepare_track(local)); // local-only tracks never contact Navidrome
	test_offline(&client,argv[1],local);
	test_playlists(&client,argv[1],database);
	test_automatic_playlists(&client,argv[1],database);
	test_navidrome_controls(&client,&cfg);
	test_deleted_slider_theme();
	test_context_downloads(&client,argv[1],local,&cfg);
	test_temporary_playback(&client,argv[1],local,database);
	test_removed_server_files(&client,argv[1],local);
	test_unchanged_offline_sync(&client,argv[1]);
	// Same title, different remote album IDs stay separate even with local
	// folder joining enabled (all remote audio lives in the same cache folder).
	library_remote_track_t same_name[2];
	assert(library_remote_get(remote.path, &same_name[0]));
	same_name[1] = same_name[0];
	strcpy(same_name[1].song.id, "different-song");
	strcpy(same_name[1].song.album_id, "different-album");
	assert(subsoniccache_path(&client, &same_name[1].song, argv[1], same_name[1].path, sizeof(same_name[1].path)));
	assert(library_remote_begin(same_name[0].source));
	assert(library_remote_stage(same_name, 2));
	assert(library_remote_publish(same_name[0].source));
	assert(album_group_count() == 3); // two remote records plus the local record

 subsonic_song_t available_song=same_name[0].song;char cached[512];
 assert(subsoniccache_fetch(&client,&available_song,argv[1],cached,sizeof(cached)));
 char cached_album[256],missing_album[256],local_album[256];
 assert(library_track_album_value(same_name[0].path,cached_album,sizeof(cached_album)));
 assert(library_track_album_value(same_name[1].path,missing_album,sizeof(missing_album)));
 assert(library_track_album_value(local,local_album,sizeof(local_album)));
 subsonic_set_network_available(false);
 assert(subsonic_album_available(cached_album) && subsonic_album_available(local_album));
 pthread_mutex_lock(&availability_probe_lock);snprintf(availability_probe_album,sizeof(availability_probe_album),"%s",missing_album);pthread_mutex_unlock(&availability_probe_lock);
 wait_album_available(missing_album,false);
 pthread_mutex_lock(&availability_probe_lock);unsigned queries_before=availability_probe_queries;pthread_mutex_unlock(&availability_probe_lock);
 for(int i=0;i<1000;++i)assert(!subsonic_album_available(missing_album));
 pthread_mutex_lock(&availability_probe_lock);assert(availability_probe_queries==queries_before);pthread_mutex_unlock(&availability_probe_lock);
 fprintf(stderr,"Offline album availability: 1000 cached checks, zero album scans.\n");
 medialist_open("Albums",LIBRARY_LIST_ALBUMS,LIBRARY_FILTER_NONE,NULL);refresh_gui();
 assert(dimmed_buttons(medialist_screen)==1);
 subsonic_set_network_available(true);assert(subsonic_ping(&client));
 wait_album_available(missing_album,true);gui_post_drain();
 assert(dimmed_buttons(medialist_screen)==0);
	library_set_join_albums(true);
	library_reorganize(); wait_scan();
	assert(album_group_count() == 3);
	assert_albums_populated();
	library_close();
	assert(library_open(database));
	assert(album_group_count() == 3);
	assert_albums_populated();
    test_connection_cleanup(&client,argv[1],local,database,settings,&same_name[0]);
	library_close();
	assert(subsonicaccount_forget(settings));
	assert(!subsonicaccount_load(settings, &restored));
	subsonic_client_clear(&client);
	puts("Account persistence and unified-library tests passed");
}
