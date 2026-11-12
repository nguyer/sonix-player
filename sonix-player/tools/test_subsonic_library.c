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
	for (int i = 0; i < 5000 && (busy || waiting_path[0]); ++i) { result_tick(NULL); usleep(1000); }
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
 auto_sync_playlists();assert(busy);pump_request();
 assert(lv_screen_active()==screen && automatic_sync_calls==calls+1);
 assert(!library_playlist_auto_pending(source,0));
 subsonic_playlist_snapshot_t remote;
 assert(subsonic_playlist_get(client,link.id,&remote));assert(remote.count==1);subsonic_playlist_free(&remote);
 assert(library_playlist_append("Automatic",&row));
 control_server(client->base_url,"test-playlist-list-fail");
 usleep(2100000);lv_tick_inc(3000);auto_retry_at=0;
 int notices=background_notices;
 auto_sync_playlists();pump_request();
 assert(library_playlist_auto_pending(source,0) && background_notices==notices+1);
 calls=automatic_sync_calls;lv_tick_inc(3000);auto_sync_playlists();
 assert(!busy && automatic_sync_calls==calls); // backoff, no tight retry loop
 auto_retry_at=0;lv_tick_inc(3000);auto_sync_playlists();pump_request();
 assert(!library_playlist_auto_pending(source,0));
 assert(subsonic_playlist_get(client,link.id,&remote));assert(remote.count==2);subsonic_playlist_free(&remote);
 // A conflicting/read-only playlist does not block other healthy server lists.
 assert(library_playlist_append("Shared",&row));assert(library_playlist_append("Automatic",&row));
 usleep(2100000);lv_tick_inc(3000);auto_retry_at=0;
 auto_sync_playlists();pump_request();
 assert(subsonic_playlist_get(client,link.id,&remote));assert(remote.count==3);subsonic_playlist_free(&remote);
 assert(!library_playlist_auto_link_pending(source,link.id));
 assert(library_playlist_auto_pending(source,0)); // Shared still needs resolution
 position=1;assert(library_playlist_remove_positions("Shared",&position,1)==1);
 assert(subsonicplaylists_sync(client,&cancel)==2);
 assert(!library_playlist_auto_pending(source,0));
 assert(library_playlist_drop("Automatic"));assert(library_playlist_auto_pending(source,0));
 usleep(2100000);lv_tick_inc(3000);auto_retry_at=0;
 auto_sync_playlists();pump_request();
 assert(!library_playlist_auto_pending(source,0));
 assert(!library_playlist_link("Automatic",&link));
 subsonic_playlist_t *lists=NULL;assert(subsonic_playlists(client,&lists)==1);free(lists);
 assert(library_playlist_exists("Local only")); // never uploaded or removed
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
		"UPDATE MEDIA_TABLE SET album_key='aold' WHERE path IN(SELECT path FROM REMOTE_MEDIA);"
		"INSERT OR IGNORE INTO ALBUM_GROUP_TABLE(album,album_key,sortkey) SELECT album,'aold',album FROM REMOTE_MEDIA;",
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
	assert(access(remote.path, F_OK) != 0);
	lv_init();
	assert(lv_display_create(480, 720));
	assert(fonts_init()); theme_init();
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
	test_playlists(&client,argv[1],database);
	test_automatic_playlists(&client,argv[1],database);
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
	library_set_join_albums(true);
	library_reorganize(); wait_scan();
	assert(album_group_count() == 3);
	assert_albums_populated();
	library_close();
	assert(library_open(database));
	assert(album_group_count() == 3);
	assert_albums_populated();
	library_close();
	assert(subsonicaccount_forget(settings));
	assert(!subsonicaccount_load(settings, &restored));
	subsonic_client_clear(&client);
	puts("Account persistence and unified-library tests passed");
}
