#include "src/system/core/lang.h"
#include "src/system/library/libraryremote.h"
#include "subsoniccache.h"
#include "src/system/core/md5.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>

static bool fail(subsonic_client_t *c, const char *message) {
	snprintf(c->error, sizeof(c->error), "%s", message);
	return false;
}

void subsoniccache_source(const subsonic_client_t *c, char out[33]) {
	subsonic_identity_t identity = {0};
	memcpy(identity.magic, "SONIXS1", 8);
	snprintf(identity.server, sizeof(identity.server), "%s", c->base_url);
	snprintf(identity.username, sizeof(identity.username), "%s", c->username);
	md5_hex(&identity, sizeof(identity), out);
}

bool subsoniccache_path(const subsonic_client_t *c, const subsonic_song_t *song,
	const char *root, char *path, size_t size) {
	if (!root || !*root || !song->id[0] || !song->suffix[0]) return false;
	char suffix[16];
	snprintf(suffix, sizeof(suffix), "%s", song->suffix);
	for (char *p = suffix; *p; ++p) {
		if (!isalnum((unsigned char)*p)) return false;
		*p = (char)tolower((unsigned char)*p);
	}
	subsonic_identity_t identity = {0};
	memcpy(identity.magic, "SONIXS1", 8);
	snprintf(identity.server, sizeof(identity.server), "%s", c->base_url);
	snprintf(identity.username, sizeof(identity.username), "%s", c->username);
	snprintf(identity.song_id, sizeof(identity.song_id), "%s", song->id);
	char hash[33];
	md5_hex(&identity, sizeof(identity), hash);
	return snprintf(path, size, "%s/.local/subsonic-cache/%s.%s", root, hash, suffix) < (int)size;
}

bool subsoniccache_identity(const char *path, subsonic_identity_t *identity) {
	char name[600];
	if (!path || !strstr(path, "/.local/subsonic-cache/") ||
		snprintf(name, sizeof(name), "%s.identity", path) >= (int)sizeof(name)) return false;
	FILE *f = fopen(name, "rb");
	if (!f) return false;
	bool ok = fread(identity, sizeof(*identity), 1, f) == 1 && fgetc(f) == EOF;
	fclose(f);
	return ok && !memcmp(identity->magic, "SONIXS1", 8) &&
		memchr(identity->server, 0, sizeof(identity->server)) &&
		memchr(identity->username, 0, sizeof(identity->username)) &&
		memchr(identity->song_id, 0, sizeof(identity->song_id));
}

static bool make_dir(const char *path) {
	if (mkdir(path, 0700) == 0) return true;
	struct stat st;
	return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void tag(FILE *f, const char *key, const char *value) {
	fprintf(f, "%s=", key);
	for (; *value; ++value) fputc(*value == '\n' || *value == '\r' ? ' ' : *value, f);
	fputc('\n', f);
}

static bool cache_fetch(subsonic_client_t *c,const subsonic_song_t *song,
 const char *root,char *path,size_t size,bool offline,int64_t allowance,int64_t reserve,bool force,const atomic_bool *cancel) {
	c->error[0] = 0;
	path[0] = 0;
	if (!root || !*root || !song->id[0]) return fail(c, tr("navidrome_music_storage_is_unavailable"));
	char suffix[sizeof(song->suffix)];
	snprintf(suffix, sizeof(suffix), "%s", song->suffix);
	for (char *p = suffix; *p; ++p) *p = (char)tolower((unsigned char)*p);
	const char *formats[] = {"mp3", "flac", "ogg", "opus", "m4a", "aac", "wav", "wv", "ape", "aiff", "aif", "alac", "dsf", "dff", "m4b", "mp4", "caf"};
	bool supported = false;
	for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i)
		if (!strcmp(suffix, formats[i])) supported = true;
	if (!supported) return fail(c, tr("navidrome_this_song_s_original_format_is_not_supported_yet"));
	char dir[440];
	if (snprintf(dir, sizeof(dir), "%s/.local", root) >= (int)sizeof(dir) || !make_dir(dir))
		return fail(c, tr("navidrome_cannot_create_the_music_cache_directory"));
	if (snprintf(dir, sizeof(dir), "%s/.local/subsonic-cache", root) >= (int)sizeof(dir) || !make_dir(dir))
		return fail(c, tr("navidrome_cannot_create_the_music_cache_directory"));
	subsonic_identity_t identity = {0};
	memcpy(identity.magic, "SONIXS1", 8);
	snprintf(identity.server, sizeof(identity.server), "%s", c->base_url);
	snprintf(identity.username, sizeof(identity.username), "%s", c->username);
	snprintf(identity.song_id, sizeof(identity.song_id), "%s", song->id);
	char key[33], target[512], part[544], tags[544], identity_path[544];
	md5_hex(&identity, sizeof(identity), key);
	if (snprintf(target, sizeof(target), "%s/%s.%s", dir, key, suffix) >= (int)sizeof(target) || strlen(target) >= size)
		return fail(c, tr("navidrome_music_cache_path_is_too_long"));
	struct stat st;
	subsonic_identity_t saved;
	if (!force && stat(target, &st) == 0 && st.st_size > 0 && subsoniccache_identity(target, &saved) &&
		!memcmp(&identity, &saved, sizeof(identity))) { strcpy(path, target); return true; }
	snprintf(part, sizeof(part), "%s.part", target);
	// Only this single serialized downloader owns .part files in this directory.
	unlink(part);
	// Reserve a whole maximum-sized track before writing. No automatic eviction
	// yet: a sweep must know which files the playback queue still needs.
	DIR *d = opendir(dir);
	if (!d) return fail(c, tr("navidrome_cannot_read_the_music_cache_directory"));
	long long bytes = 0;
	struct dirent *entry;
	while ((entry = readdir(d))) {
		char file[768];
		snprintf(file, sizeof(file), "%s/%s", dir, entry->d_name);
  char pin[800];snprintf(pin,sizeof(pin),"%s",file);
  size_t length=strlen(pin);
  if(length>8 && !strcmp(pin+length-8,".offline"))continue;
  if(length>9 && !strcmp(pin+length-9,".identity"))pin[length-9]=0;
  else if(length>5 && !strcmp(pin+length-5,".tags"))pin[length-5]=0;
  strncat(pin,".offline",sizeof(pin)-strlen(pin)-1);
  if(access(pin,F_OK)==0)continue;
  if (stat(file, &st) == 0 && S_ISREG(st.st_mode)) bytes += st.st_size;
	}
	closedir(d);
 if(!offline && bytes>512LL*1024*1024)return fail(c,tr("navidrome_song_cache_is_full_remove_old_cached_songs_before_downloading_more"));
 if(offline){if(!subsonic_download_limited(c,song->id,part,allowance,reserve,cancel))return false;}
 else if(!subsonic_download(c,song->id,part))return false;
 char final_tags[544],final_identity[544];
 snprintf(final_tags,sizeof(final_tags),"%s.tags",target);snprintf(final_identity,sizeof(final_identity),"%s.identity",target);
 snprintf(tags,sizeof(tags),"%s.tags.tmp",target);snprintf(identity_path,sizeof(identity_path),"%s.identity.tmp",target);
	FILE *f = fopen(tags, "w");
	bool ok = f != NULL;
	if (f) {
		tag(f, "title", song->title); tag(f, "artist", song->artist); tag(f, "album", song->album);
		tag(f, "album_artist", song->album_artist); tag(f, "genre", song->genre);
		fprintf(f, "year=%d\ndisc=%d\n", song->year, song->disc > 0 ? song->disc : 1);
		fprintf(f, "track=%d\n", song->track);
		if (ferror(f)) ok = false;
		if (fclose(f) != 0) ok = false;
	}
	f = ok ? fopen(identity_path, "wb") : NULL;
	if (f) {
		if (fwrite(&identity, sizeof(identity), 1, f) != 1) ok = false;
		if (fclose(f) != 0) ok = false;
	} else ok = false;
	if (!ok || rename(tags,final_tags)!=0 || rename(identity_path,final_identity)!=0 || rename(part, target) != 0) {
		unlink(part); unlink(tags); unlink(identity_path);
		return fail(c, tr("navidrome_cannot_finish_saving_the_downloaded_song"));
	}
	strcpy(path, target);
	return true;
}

bool subsoniccache_fetch(subsonic_client_t *c,const subsonic_song_t *song,const char *root,char *path,size_t size) {
 return cache_fetch(c,song,root,path,size,false,512LL*1024*1024,0,false,NULL);
}
bool subsoniccache_fetch_offline(subsonic_client_t *c,const subsonic_song_t *song,const char *root,char *path,size_t size,
 int64_t allowance,int64_t reserve,bool force,const atomic_bool *cancel) {
 return cache_fetch(c,song,root,path,size,true,allowance,reserve,force,cancel);
}

bool subsoniccache_is_temporary(const char *path){
 char temporary[560],pin[560];snprintf(temporary,sizeof(temporary),"%s.temporary",path);snprintf(pin,sizeof(pin),"%s.offline",path);
 return access(temporary,F_OK)==0 && access(pin,F_OK)!=0;
}
bool subsoniccache_mark_temporary(const char *path){
 char marker[560];snprintf(marker,sizeof(marker),"%s.offline",path);if(access(marker,F_OK)==0)return true;
 snprintf(marker,sizeof(marker),"%s.temporary",path);FILE *f=fopen(marker,"w");if(!f)return false;
 bool ok=fputs("Temporary playback download\n",f)>=0;if(fclose(f)!=0)ok=false;return ok;
}
bool subsoniccache_discard_temporary(const subsonic_client_t *client,const char *root,const char *path){
 char marker[560];snprintf(marker,sizeof(marker),"%s.temporary",path);
 if(access(marker,F_OK)!=0)return true;
 subsonic_identity_t identity;
 if(!subsoniccache_identity(path,&identity) || strcmp(identity.server,client->base_url) || strcmp(identity.username,client->username))return false;
 subsonic_song_t song={0};snprintf(song.id,sizeof(song.id),"%s",identity.song_id);
 const char *dot=strrchr(path,'.');if(!dot || strlen(dot+1)>=sizeof(song.suffix))return false;strcpy(song.suffix,dot+1);
 char expected[512];if(!subsoniccache_path(client,&song,root,expected,sizeof(expected)) || strcmp(path,expected))return false;
 char pin[560];snprintf(pin,sizeof(pin),"%s.offline",path);
 if(access(pin,F_OK)==0){unlink(marker);return true;}
 return subsoniccache_remove_owned(client,root,path,identity.song_id);
}
void subsoniccache_cleanup_temporary(const subsonic_client_t *client,const char *root,const char *keep){
 char directory[560];snprintf(directory,sizeof(directory),"%s/.local/subsonic-cache",root);
 DIR *dir=opendir(directory);if(!dir)return;struct dirent *entry;
 while((entry=readdir(dir))){
  size_t n=strlen(entry->d_name);if(n<=10 || strcmp(entry->d_name+n-10,".temporary"))continue;
  char path[512];int length=snprintf(path,sizeof(path),"%s/%.*s",directory,(int)(n-10),entry->d_name);
  if(length<0 || length>=(int)sizeof(path) || (keep && !strcmp(keep,path)))continue;
  subsoniccache_discard_temporary(client,root,path);
 }
 closedir(dir);
}

bool subsoniccache_remove_owned(const subsonic_client_t *client,const char *root,const char *path,const char *id){
 subsonic_song_t song={0};snprintf(song.id,sizeof(song.id),"%s",id);
 const char *dot=strrchr(path,'.');if(!dot || strlen(dot+1)>=sizeof(song.suffix))return false;strcpy(song.suffix,dot+1);
 char expected[512];if(!subsoniccache_path(client,&song,root,expected,sizeof(expected)) || strcmp(expected,path))return false;
 struct stat st;bool exists=lstat(path,&st)==0;
 if(exists && !S_ISREG(st.st_mode))return false;
 if(!exists && errno!=ENOENT)return false;
 const char *suffixes[]={".offline",".temporary",".part",".tags.tmp",".identity.tmp",".tags",".identity"};
 subsonic_identity_t identity;
 if(!subsoniccache_identity(path,&identity)){
  library_remote_track_t remote;
  bool catalog=library_remote_get(path,&remote) && !strcmp(remote.server,client->base_url) &&
   !strcmp(remote.username,client->username) && !strcmp(remote.song.id,id);
  if(!catalog){
   if(exists)return false;
   for(unsigned i=0;i<sizeof(suffixes)/sizeof(suffixes[0]);++i){char side[560];snprintf(side,sizeof(side),"%s%s",path,suffixes[i]);if(lstat(side,&st)==0 || errno!=ENOENT)return false;}
   return true;
  }
 }else if(strcmp(identity.server,client->base_url) || strcmp(identity.username,client->username) || strcmp(identity.song_id,id))return false;
 if(exists && unlink(path)!=0)return false;
 const char *lyrics[]={".lrc",".lrc.part"};
 for(unsigned i=0;i<sizeof(lyrics)/sizeof(lyrics[0]);i++){
  char side[560];snprintf(side,sizeof(side),"%.*s%s",(int)(dot-path),path,lyrics[i]);
  if(unlink(side)!=0 && errno!=ENOENT)return false;
 }
 // Keep identity until last: failed/interrupted removal can be retried safely.
 for(unsigned i=0;i<sizeof(suffixes)/sizeof(suffixes[0]);++i){
  char side[560];snprintf(side,sizeof(side),"%s%s",path,suffixes[i]);if(unlink(side)!=0 && errno!=ENOENT)return false;
 }
 return true;
}
