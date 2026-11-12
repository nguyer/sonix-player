#include <pthread.h>
#include <stdlib.h>
#include "src/system/core/utils.h"
#include "subsonicoffline.h"
#include <stdio.h>
#include "subsonicavailability.h"
#include "src/system/library/library.h"
#include "src/system/library/libraryremote.h"
#include <string.h>
#include <sys/stat.h>
bool subsonic_track_available(const char *path){
 if(!path || !strstr(path,"/.local/subsonic-cache/"))return true;
 library_remote_track_t track;
 if(!library_remote_get(path,&track)){struct stat local;return stat(path,&local)==0 && S_ISREG(local.st_mode) && local.st_size>0;}
 struct stat st;
 if(stat(path,&st)==0 && S_ISREG(st.st_mode) && st.st_size>0)return true;
 return subsonic_server_available(track.server,track.username);
}


typedef struct {char paths[16][512];int count;} album_window_t;
static bool album_available_row(const char *name,const char *path,const char *artist,void *user){
 (void)name;(void)artist;album_window_t *window=user;
 if(path && window->count<16)snprintf(window->paths[window->count++],512,"%s",path);
 return true;
}
static bool compute_album_available(const char *album){
 if(!album || !*album)return true;
 library_index_t *index=library_index_open(LIBRARY_LIST_TRACKS,LIBRARY_FILTER_ALBUM,album,LIBRARY_ORDER_DEFAULT,false);
 if(!index)return true;
 bool available=false;int count=library_index_count(index);
 for(int first=0;first<count && !available;first+=16){
  album_window_t window={0};library_index_window(index,first,16,album_available_row,&window);
  // The library invokes callbacks under its lock; remote lookups must run after it releases.
  for(int i=0;i<window.count && !available;++i)available=subsonic_track_available(window.paths[i]);
 }
 library_index_close(index);return available;
}

// The UI only touches this small cache. SQLite, directory access and album
// scans belong to a lower-priority worker, never to row binding or drawing.
#define ALBUM_AVAILABILITY_SLOTS 128
typedef struct {char album[256];unsigned epoch,requested;unsigned long used;bool value,ready,queued,computing;} album_entry_t;
static album_entry_t albums[ALBUM_AVAILABILITY_SLOTS];
static pthread_mutex_t albums_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t albums_wake=PTHREAD_COND_INITIALIZER;
static unsigned album_epoch=1,last_library_revision,last_network_revision,last_download_revision;
static unsigned long album_clock;
static bool worker_started;
static void (*album_notify)(void);
void subsonicavailability_set_notify(void (*notify)(void)){pthread_mutex_lock(&albums_lock);album_notify=notify;pthread_mutex_unlock(&albums_lock);}
static void *album_worker(void *unused){
 (void)unused;thread_be_background("album availability");
 for(;;){
  pthread_mutex_lock(&albums_lock);int slot=-1;
  for(;;){
   for(int i=0;i<ALBUM_AVAILABILITY_SLOTS;++i)if(albums[i].queued && !albums[i].computing){slot=i;break;}
   if(slot>=0)break;
   pthread_cond_wait(&albums_wake,&albums_lock);
  }
  album_entry_t *entry=&albums[slot];entry->computing=true;entry->queued=false;
  unsigned requested=entry->requested;char key[256];snprintf(key,sizeof(key),"%s",entry->album);
  pthread_mutex_unlock(&albums_lock);
  bool value=compute_album_available(key);bool published=false;
  pthread_mutex_lock(&albums_lock);entry=&albums[slot];entry->computing=false;
  if(requested==album_epoch && !strcmp(key,entry->album)){
   entry->value=value;entry->ready=true;entry->epoch=requested;published=true;
   if(entry->requested==requested)entry->queued=false;
  }
  void (*notify)(void)=album_notify;
  pthread_mutex_unlock(&albums_lock);if(published && notify)notify();
 }
 return NULL;
}
void subsonicavailability_invalidate(void){
 pthread_mutex_lock(&albums_lock);++album_epoch;pthread_mutex_unlock(&albums_lock);
}
void subsonicavailability_refresh(void){
 unsigned library=library_revision(LIBRARY_LIST_ALBUMS),network=subsonic_availability_revision(),download=subsonicoffline_revision();
 pthread_mutex_lock(&albums_lock);
 if(library!=last_library_revision || network!=last_network_revision || download!=last_download_revision){
  last_library_revision=library;last_network_revision=network;last_download_revision=download;++album_epoch;
 }
 pthread_mutex_unlock(&albums_lock);
}
bool subsonic_album_available(const char *album){
 if(!album || !*album)return true;
 pthread_mutex_lock(&albums_lock);int slot=-1,oldest=-1;
 for(int i=0;i<ALBUM_AVAILABILITY_SLOTS;++i){
  if(!strcmp(albums[i].album,album)){slot=i;break;}
  if(!albums[i].computing && !albums[i].queued && (oldest<0 || albums[i].used<albums[oldest].used))oldest=i;
 }
 if(slot<0){slot=oldest;if(slot<0){pthread_mutex_unlock(&albums_lock);return true;}
  memset(&albums[slot],0,sizeof(albums[slot]));snprintf(albums[slot].album,256,"%s",album);
 }
 album_entry_t *entry=&albums[slot];entry->used=++album_clock;
 bool value=entry->ready?entry->value:true;
 if(!entry->ready || entry->epoch!=album_epoch){
  entry->requested=album_epoch;entry->queued=true;pthread_cond_signal(&albums_wake);
  if(!worker_started){pthread_t thread;if(pthread_create(&thread,NULL,album_worker,NULL)==0){worker_started=true;pthread_detach(thread);}}
 }
 pthread_mutex_unlock(&albums_lock);return value;
}
