#include "subsonicart.h"
#include "src/system/library/libraryremote.h"
#include "subsonicoffline.h"
#include "subsoniccache.h"
#include "subsoniccatalog.h"
#include "subsonicplaylistapi.h"
#include "subsonicinternal.h"
#include "streamturn.h"
#include "src/system/library/libraryoffline.h"
#include "src/system/device/system.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

static atomic_bool running,cancelled,interrupted,yielding,transferring;
static atomic_uint yield_serial,revision;
unsigned subsonicoffline_revision(void){return atomic_load(&revision);}
static pthread_mutex_t status_lock=PTHREAD_MUTEX_INITIALIZER;
static subsonic_offline_status_t status;
typedef struct {subsonic_client_t client;char root[512],source[33];offline_options_t options;bool refresh;} job_t;
static void (*job_notify)(void);
void subsonicoffline_set_notify(void (*notify)(void)){pthread_mutex_lock(&status_lock);job_notify=notify;pthread_mutex_unlock(&status_lock);}
static void notify_job(void){pthread_mutex_lock(&status_lock);void (*notify)(void)=job_notify;pthread_mutex_unlock(&status_lock);if(notify)notify();}
static void message(const char *text) {pthread_mutex_lock(&status_lock);snprintf(status.message,sizeof(status.message),"%s",text);pthread_mutex_unlock(&status_lock);notify_job();}
void subsonicoffline_status(subsonic_offline_status_t *out){pthread_mutex_lock(&status_lock);*out=status;pthread_mutex_unlock(&status_lock);out->running=atomic_load(&running);}
bool subsonicoffline_busy(void){return atomic_load(&running);}
void subsonicoffline_cancel(void){atomic_store(&cancelled,true);atomic_store(&interrupted,true);if(atomic_load(&transferring))subsonic_download_cancel();}
void subsonicoffline_yield_begin(void){atomic_fetch_add(&yield_serial,1);atomic_store(&yielding,true);atomic_store(&interrupted,true);if(atomic_load(&transferring))subsonic_download_cancel();}
void subsonicoffline_yield_end(void){atomic_store(&yielding,false);if(!atomic_load(&cancelled))atomic_store(&interrupted,false);}
static bool extra_add(char (**ids)[128],int *count,const char (*more)[128],int n){
 if(n<0 || *count+n>20000)return false;
 void *p=realloc(*ids,(size_t)(*count+n+1)*128);if(!p)return false;*ids=p;
 if(n)memcpy(*ids+*count,more,(size_t)n*128);
 *count+=n;return true;
}
static bool extras(job_t *job,char (**ids)[128],int *count){
 *ids=NULL;*count=0;
 offline_choice_t choices[20];
 for(int offset=0;;offset+=20){
  int n=library_offline_choices(job->source,OFFLINE_PLAYLIST,"",offset,choices,20);if(n<0)return false;
  for(int i=0;i<n;++i)if(choices[i].selected){
   if(atomic_load(&cancelled))return false;
   subsonic_playlist_snapshot_t p;if(!subsonic_playlist_get(&job->client,choices[i].value,&p))return false;
   bool ok=extra_add(ids,count,(const char (*)[128])p.ids,p.count);subsonic_playlist_free(&p);if(!ok)return false;
  }
  if(n<20)break;
 }
 if(library_offline_has_rule(job->source,OFFLINE_FAVORITES,"1")){
  char *body;json_doc_t doc;int root;
  if(!subsonic_api_call(&job->client,"getStarred2","",&body,&doc,&root))return false;
  int group=json_get(&doc,root,"starred2"),songs=json_get(&doc,group,"song"),n=json_len(&doc,songs);
  bool ok=group>=0 && doc.toks[group].type==JSON_OBJECT && n<=10000 && (songs<0 || doc.toks[songs].type==JSON_ARRAY);
  char (*more)[128]=ok?calloc((size_t)n+1,128):NULL;if(!more)ok=false;
  for(int i=0;ok && i<n;++i){int id=json_get(&doc,json_at(&doc,songs,i),"id");ok=id>=0 && doc.toks[id].type==JSON_STRING && doc.toks[id].end-doc.toks[id].start<128 && json_str(&doc,id,more[i],128) && more[i][0];}
  if(ok)ok=extra_add(ids,count,(const char (*)[128])more,n);
  free(more);json_free(&doc);free(body);if(!ok)return false;
 }
 return true;
}
static bool owned(job_t *job,const offline_file_t *file,struct stat *st){
 subsonic_identity_t identity;
 if(lstat(file->path,st)!=0 || !S_ISREG(st->st_mode) || !subsoniccache_identity(file->path,&identity))return false;
 if(strcmp(identity.server,job->client.base_url) || strcmp(identity.username,job->client.username) || strcmp(identity.song_id,file->id))return false;
 subsonic_song_t song={0};snprintf(song.id,sizeof(song.id),"%s",file->id);
 const char *dot=strrchr(file->path,'.');if(!dot || strlen(dot+1)>=sizeof(song.suffix))return false;strcpy(song.suffix,dot+1);
 char expected[512];return subsoniccache_path(&job->client,&song,job->root,expected,sizeof(expected)) && !strcmp(expected,file->path);
}
static bool pin(const char *path){char marker[560];snprintf(marker,sizeof(marker),"%s.offline",path);FILE *f=fopen(marker,"w");if(!f)return false;bool ok=fputs("SONIX-OFFLINE-1\n",f)>=0;if(fclose(f)!=0)ok=false;if(ok){snprintf(marker,sizeof(marker),"%s.temporary",path);unlink(marker);}return ok;}
static void cleanup(job_t *job){
 for(int offset=0;!atomic_load(&cancelled);){
  offline_file_t files[20];int n=library_offline_files(job->source,offset,files,20),removed=0;if(n<=0)break;
  for(int i=0;i<n;++i)if(!files[i].wanted && !library_offline_path_protected(files[i].path)){
   struct stat st;char marker[560];snprintf(marker,sizeof(marker),"%s.offline",files[i].path);
   if(access(files[i].path,F_OK)!=0){if(library_offline_forget(job->source,files[i].id))++removed;}
   else if(access(marker,F_OK)==0 && owned(job,&files[i],&st) && unlink(files[i].path)==0){
    const char *suffixes[]={".offline",".tags",".identity"};
    for(int k=0;k<3;++k){snprintf(marker,sizeof(marker),"%s%s",files[i].path,suffixes[k]);unlink(marker);}
    if(library_offline_forget(job->source,files[i].id))++removed;
   }
  }
  offset+=n-removed;if(n<20)break;
 }
}
static long long offline_millis(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000+t.tv_nsec/1000000;}
static void *worker(void *arg){
 job_t *job=arg;bool ok=true;long long began=offline_millis();int cached=0,fetches=0;
 message(job->refresh?"Refreshing Navidrome catalog...":"Checking offline downloads...");
 if(job->refresh && subsoniccatalog_sync(&job->client,job->root,&cancelled)<0)ok=false;
 char (*ids)[128]=NULL;int extra_count=0;
 if(ok && job->options.mode==OFFLINE_SELECTIVE)ok=extras(job,&ids,&extra_count);
 if(ok)ok=library_offline_plan(job->source,job->options.mode==OFFLINE_MIRROR,(const char (*)[128])ids,extra_count);
 free(ids);
 if(!ok){message(job->client.error[0]?job->client.error:"Cannot prepare offline sync. Previous downloads were kept.");goto finished;}
 // Reconcile actual storage before enforcing the budget. Unpinned cached files
 // are promoted only after their size fits, rather than silently exceeding it.
 int total=0;
 for(int offset=0;;offset+=20){
  offline_file_t files[20];int n=library_offline_files(job->source,offset,files,20);if(n<0){ok=false;break;}
  for(int i=0;i<n;++i){
   if(files[i].wanted)++total;
   if(files[i].bytes>0){
    struct stat st;bool valid=owned(job,&files[i],&st);
    long long bytes=valid?st.st_size:0;int state=valid?2:0;
    if((files[i].bytes!=bytes || files[i].status!=state) && !library_offline_note(job->source,files[i].id,bytes,files[i].stamp,state)){ok=false;message("Cannot update offline storage accounting.");break;}
   }
  }
  if(n<20)break;
 }
 pthread_mutex_lock(&status_lock);status.total=total;pthread_mutex_unlock(&status_lock);
 if(ok && job->options.cleanup)cleanup(job);
 long long used=library_offline_bytes(job->source);
 for(int offset=0;ok && !atomic_load(&cancelled);offset+=20){
  offline_file_t files[20];int n=library_offline_files(job->source,offset,files,20);if(n<0){ok=false;break;}
  for(int i=0;ok && i<n && !atomic_load(&cancelled);++i)if(files[i].wanted){
   if(!files[i].catalog_present){ok=false;message("Catalog changed during offline sync. Try again.");break;}
   struct stat st;bool valid=owned(job,&files[i],&st);
   bool force=valid && files[i].bytes>0 && files[i].remote_bytes>0 && files[i].stamp!=files[i].remote_bytes;
   long long allowance=job->options.max_bytes?job->options.max_bytes-used+files[i].bytes:512LL*1024*1024;
   if((valid && !force && job->options.max_bytes && used-files[i].bytes+st.st_size>job->options.max_bytes) ||
    (!valid && allowance<=0) || (files[i].remote_bytes>0 && (!valid || force) && files[i].remote_bytes>allowance)){
    message("Offline storage limit reached. Existing downloads were kept.");ok=false;break;
   }
   char marker[560];snprintf(marker,sizeof(marker),"%s.offline",files[i].path);
   if(valid && !force && access(marker,F_OK)==0){
    if((files[i].bytes!=st.st_size || files[i].stamp!=files[i].remote_bytes || files[i].status!=2) &&
     !library_offline_note(job->source,files[i].id,st.st_size,files[i].remote_bytes,2)){ok=false;message("Cannot record offline download.");break;}
    used+=st.st_size-files[i].bytes;++cached;
    pthread_mutex_lock(&status_lock);++status.done;status.bytes=used;pthread_mutex_unlock(&status_lock);
    if(cached%20==0)notify_job();
    continue;
   }
   library_remote_track_t track;if(!library_remote_get(files[i].path,&track)){ok=false;message("Catalog changed during offline sync. Try again.");break;}
   pthread_mutex_lock(&status_lock);snprintf(status.path,sizeof(status.path),"%s",files[i].path);pthread_mutex_unlock(&status_lock);
   char text[256],path[512];snprintf(text,sizeof(text),"Downloading: %.210s",track.song.title);message(text);
   bool fetched=false;
   while(!fetched && !atomic_load(&cancelled)){
    while(atomic_load(&yielding) && !atomic_load(&cancelled))usleep(100000);
    if(atomic_load(&cancelled))break;
    unsigned serial=atomic_load(&yield_serial);
    streamturn_acquire();
    if(atomic_load(&yielding) || atomic_load(&cancelled)){streamturn_release();continue;}
    atomic_store(&transferring,true);
    ++fetches;
    fetched=subsoniccache_fetch_offline(&job->client,&track.song,job->root,path,sizeof(path),allowance,job->options.reserve_bytes,force,&interrupted);
    atomic_store(&transferring,false);streamturn_release();
    if(!fetched && serial==atomic_load(&yield_serial)){ok=false;message(job->client.error);break;}
   }
   if(fetched){
    if(stat(path,&st)!=0 || !pin(path) || !library_offline_note(job->source,files[i].id,st.st_size,track.song.size_bytes,2)){ok=false;message("Cannot record offline download. Try again.");break;}
    used+=st.st_size-files[i].bytes;
    pthread_mutex_lock(&status_lock);++status.done;status.bytes=used;status.path[0]=0;pthread_mutex_unlock(&status_lock);notify_job();
   }
  }
  if(n<20)break;
 }
 if(atomic_load(&cancelled))message("Offline sync stopped. Completed downloads were kept.");
 else if(ok){library_offline_complete(job->source,time(NULL));message("Offline sync complete.");}
finished:
 fprintf(stderr,"Navidrome offline sync: %d cached, %d fetch/promote operations, %lld ms%s\n",cached,fetches,offline_millis()-began,atomic_load(&cancelled)?" (cancelled)":"");
 subsonic_client_clear(&job->client);free(job);atomic_fetch_add(&revision,1);atomic_store(&running,false);notify_job();return NULL;
}
bool subsonicoffline_start(const subsonic_client_t *client,const char *root,bool refresh){
 if(!root || !*root || strlen(root)>=512 || !client->password[0])return false;
 bool expected=false;if(!atomic_compare_exchange_strong(&running,&expected,true))return false;
 job_t *job=calloc(1,sizeof(*job));if(!job){atomic_store(&running,false);notify_job();return false;}
 job->client=*client;strcpy(job->root,root);subsoniccache_source(client,job->source);job->refresh=refresh;
 if(!library_offline_options(job->source,&job->options) || job->options.mode==OFFLINE_OFF){
  message("Offline sync is off.");subsonic_client_clear(&job->client);free(job);atomic_store(&running,false);notify_job();return false;
 }
 atomic_store(&cancelled,false);atomic_store(&interrupted,false);atomic_store(&yielding,false);
 pthread_mutex_lock(&status_lock);memset(&status,0,sizeof(status));strcpy(status.message,"Starting offline sync...");strcpy(status.source,job->source);pthread_mutex_unlock(&status_lock);
 pthread_t thread;
 if(pthread_create(&thread,NULL,worker,job)!=0){subsonic_client_clear(&job->client);free(job);atomic_store(&running,false);notify_job();return false;}
 pthread_detach(thread);notify_job();return true;
}

typedef struct {job_t job;char (*paths)[512];int count;bool all;} remove_job_t;
static bool remove_file(job_t *job,const char *path){
 subsonic_identity_t identity;
 if(!subsoniccache_identity(path,&identity)) {
  library_remote_track_t track;
  if(!library_remote_get(path,&track) || strcmp(track.source,job->source))return true;
  snprintf(identity.server,sizeof(identity.server),"%s",track.server);
  snprintf(identity.username,sizeof(identity.username),"%s",track.username);
  snprintf(identity.song_id,sizeof(identity.song_id),"%s",track.song.id);
 }
 if(strcmp(identity.server,job->client.base_url) || strcmp(identity.username,job->client.username))return true;
 return subsoniccache_remove_owned(&job->client,job->root,path,identity.song_id) &&
  library_offline_forget(job->source,identity.song_id);
}
static void *remove_worker(void *arg){
 remove_job_t *r=arg;bool ok=true;
 streamturn_acquire(); // no foreground cache publish can race deletion
 if(r->all){
  for(int offset=0;;) {
   library_remote_removed_t rows[32];int n=library_remote_files(r->job.source,offset,rows,32);
   if(n<0){ok=false;break;}
   for(int i=0;i<n;i++)if(!remove_file(&r->job,rows[i].path))ok=false;
   offset+=n;if(n<32)break;
  }
  char directory[560];snprintf(directory,sizeof(directory),"%s/.local/subsonic-cache",r->job.root);
  DIR *dir=opendir(directory);
  if(!dir && errno!=ENOENT)ok=false;
  if(dir){struct dirent *entry;while((entry=readdir(dir))){
   size_t n=strlen(entry->d_name);if(n<=9 || strcmp(entry->d_name+n-9,".identity"))continue;
   char path[512];int length=snprintf(path,sizeof(path),"%s/%.*s",directory,(int)(n-9),entry->d_name);
   if(length<0 || length>=(int)sizeof(path)){ok=false;continue;}
   if(!remove_file(&r->job,path))ok=false;
  }closedir(dir);}
 }else for(int i=0;i<r->count;++i)if(!remove_file(&r->job,r->paths[i]))ok=false;
 if(!subsonicart_prune(&r->job.client,r->job.root))ok=false;
 streamturn_release();
 message(ok?"Downloads removed.":"Some downloads could not be removed. Try again.");
 pthread_mutex_lock(&status_lock);status.removing=false;pthread_mutex_unlock(&status_lock);
 free(r->paths);subsonic_client_clear(&r->job.client);free(r);atomic_fetch_add(&revision,1);atomic_store(&running,false);notify_job();return NULL;
}
bool subsonicoffline_remove(const subsonic_client_t *client,const char *root,const char *const *paths,int count,bool all){
 if(!client || !root || !*root || strlen(root)>=512 || count<0 || (!all && (!paths || count<1)))return false;
 bool expected=false;if(!atomic_compare_exchange_strong(&running,&expected,true))return false;
 remove_job_t *r=calloc(1,sizeof(*r));if(!r){atomic_store(&running,false);notify_job();return false;}
 r->job.client=*client;strcpy(r->job.root,root);subsoniccache_source(client,r->job.source);r->all=all;r->count=count;
 if(count){r->paths=calloc((size_t)count,sizeof(*r->paths));if(!r->paths)goto failed;
  for(int i=0;i<count;++i){if(!paths[i] || strlen(paths[i])>=512)goto failed;strcpy(r->paths[i],paths[i]);}}
 pthread_mutex_lock(&status_lock);memset(&status,0,sizeof(status));status.removing=true;pthread_mutex_unlock(&status_lock);
 message("Removing downloads...");
 pthread_t thread;if(pthread_create(&thread,NULL,remove_worker,r)!=0)goto failed;
 pthread_detach(thread);notify_job();return true;
failed:
 free(r->paths);subsonic_client_clear(&r->job.client);free(r);atomic_store(&running,false);notify_job();return false;
}
