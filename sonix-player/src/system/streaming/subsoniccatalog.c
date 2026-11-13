#include "src/system/core/lang.h"
#include "streamturn.h"
#include "src/system/audio/audio.h"
#include "subsoniccatalog.h"
#include "subsoniccache.h"
#include "subsonicart.h"
#include "src/system/library/libraryremote.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long long catalog_millis(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000+t.tv_nsec/1000000;}
static atomic_bool syncing;
bool subsoniccatalog_busy(void) { return atomic_load(&syncing); }

static int sync_covers(subsonic_client_t *c, const char *root, const char *source, const atomic_bool *cancel) {
	char ids[50][128];
	int offset = 0, missing = 0;
	for (;;) {
		int count = library_remote_covers(source, offset, ids, 50);
		if (count < 0) return -1;
		for (int i = 0; i < count; ++i) {
			if (atomic_load(cancel)) return -1;
			if (!subsonicart_fetch(c, ids[i], root)) ++missing;
		}
		offset += count;
		if (count < 50) break;
	}
	return missing;
}

int subsoniccatalog_sync(subsonic_client_t *c, const char *root, const atomic_bool *cancel) {
	long long began=catalog_millis(),fetch_ms=0,art_ms=0,publish_ms=0,cleanup_ms=0;
	char source[33];
	subsoniccache_source(c, source);
	if (!root || !*root || !library_remote_begin(source)) {
		snprintf(c->error,sizeof(c->error),"%s",tr("navidrome_library_is_unavailable_or_scanning_try_syncing_again_when_it_finishes"));
		return -1;
	}
	atomic_store(&syncing, true);
	subsonic_song_t *songs = calloc(50, sizeof(*songs));
	library_remote_track_t *rows = calloc(50, sizeof(*rows));
	int total = 0;
	if (!songs || !rows) { snprintf(c->error,sizeof(c->error),"%s",tr("navidrome_not_enough_memory_to_sync_the_library")); total = -1; }
	while (total >= 0) {
		if (atomic_load(cancel)) { snprintf(c->error,sizeof(c->error),"%s",tr("navidrome_library_sync_cancelled")); total = -1; break; }
		// Navidrome's search3 with an empty query enumerates all songs, in pages.
		int count = subsonic_search_songs(c, "", total, songs, 50);
		if (count < 0) { total = -1; break; }
		memset(rows, 0, 50 * sizeof(*rows));
		bool valid = true;
		for (int i = 0; i < count; ++i) {
			rows[i].song = songs[i];
			strcpy(rows[i].source, source); strcpy(rows[i].server, c->base_url); strcpy(rows[i].username, c->username);
			if (!subsoniccache_path(c, &songs[i], root, rows[i].path, sizeof(rows[i].path))) valid = false;
		}
		if (!valid || (count && !library_remote_stage(rows, count))) {
			snprintf(c->error,sizeof(c->error),"%s",tr("navidrome_cannot_save_the_catalog_check_storage_and_wait_for_any_library_scan"));
			total = -1; break;
		}
		total += count;
        if(count<50){
         fetch_ms=catalog_millis()-began;long long phase=catalog_millis();
         int missing=sync_covers(c,root,source,cancel);art_ms=catalog_millis()-phase;phase=catalog_millis();
         bool published=missing>=0 && !atomic_load(cancel) && library_remote_publish(source);
         publish_ms=catalog_millis()-phase;phase=catalog_millis();
         if(!published){snprintf(c->error,sizeof(c->error),"%s",tr("navidrome_library_sync_was_interrupted_the_previous_catalog_is_unchanged"));total=-1;}
         else if(subsoniccatalog_cleanup(c,root,cancel)<0)total=-1;
         else if(missing)snprintf(c->error,sizeof(c->error),tr("navidrome_covers_unavailable"),missing);
         else c->error[0]=0;
         cleanup_ms=catalog_millis()-phase;
			break;
		}
	}
	free(rows); free(songs);
	fprintf(stderr,"Navidrome catalog sync: %d tracks, %lld ms (fetch %lld, artwork %lld, publish %lld, cleanup %lld)\n",total,catalog_millis()-began,fetch_ms,art_ms,publish_ms,cleanup_ms);
	atomic_store(&syncing, false);
	return total;
}

int subsoniccatalog_cleanup(subsonic_client_t *client,const char *root,const atomic_bool *cancel){
 char source[33];subsoniccache_source(client,source);int removed=0;bool ok=true;
 streamturn_acquire();
 for(int offset=0;!atomic_load(cancel);){
  library_remote_removed_t rows[20];int n=library_remote_removed(source,offset,rows,20),completed=0;
  if(n<0){ok=false;break;}if(!n)break;
  for(int i=0;i<n && !atomic_load(cancel);++i){
   char current[512];audio_get_current_file(current,sizeof(current));
   if(audio_get_status()!=AUDIO_STATUS_STOPPED && !strcmp(current,rows[i].path))continue;
   if(!subsoniccache_remove_owned(client,root,rows[i].path,rows[i].id) || !library_remote_removed_done(source,rows[i].path)){ok=false;continue;}
   ++completed;++removed;
  }
  offset+=n-completed;if(n<20)break;
 }
 streamturn_release();
 if(!ok){snprintf(client->error,sizeof(client->error),"%s",tr("navidrome_catalog_updated_but_removed_downloads_could_not_all_be_cleaned_up"));return -1;}
 return removed;
}
