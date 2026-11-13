#include "subsonicart.h"
#include "subsoniccache.h"
#include "src/system/library/libraryremote.h"
#include "src/system/core/md5.h"
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool art_path(const char *root, const char *source, const char *id, char *out, size_t size) {
	if (!root || !*root || !id || !*id) return false;
	char hash[33];
	md5_hex(id, strlen(id), hash);
	return snprintf(out, size, "%s/.local/subsonic-art/%s-%s.jpg", root, source, hash) < (int)size;
}

static bool exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool directory(const char *path) {
	struct stat st;
	return mkdir(path, 0700) == 0 || (errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

bool subsonicart_fetch(subsonic_client_t *c, const char *id, const char *root) {
	char source[33], path[640], dir[560], temporary[680];
	subsoniccache_source(c, source);
	if (!art_path(root, source, id, path, sizeof(path))) return false;
	if (exists(path)) return true;
	if (snprintf(dir, sizeof(dir), "%s/.local", root) >= (int)sizeof(dir) || !directory(dir)) return false;
	if (snprintf(dir, sizeof(dir), "%s/.local/subsonic-art", root) >= (int)sizeof(dir) || !directory(dir)) return false;
	char *data = NULL;
	size_t size = 0;
	if (!subsonic_cover(c, id, &data, &size)) return false;
	snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
	int fd = mkstemp(temporary);
	FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
	bool ok = f != NULL;
	if (f) {
		if (fwrite(data, 1, size, f) != size) ok = false;
		if (fclose(f) != 0) ok = false;
	} else if (fd >= 0) close(fd);
	free(data);
	if (ok) ok = rename(temporary, path) == 0;
	if (!ok && fd >= 0) unlink(temporary);
	return ok;
}

bool subsonicart_for_track(const char *path, char *out, size_t size) {
	if (!path) return false;
	const char *cache = strstr(path, "/.local/subsonic-cache/");
	if (!cache || cache - path >= 512) return false;
	library_remote_track_t remote;
	if (!library_remote_get(path, &remote)) return false;
	char root[512];
	memcpy(root, path, (size_t)(cache - path)); root[cache - path] = 0;
	const char *id = remote.song.cover_id[0] ? remote.song.cover_id : remote.song.album_id;
	return art_path(root, remote.source, id, out, size) && exists(out);
}

static int compare_hash(const void *a,const void *b){return strcmp(a,b);}
bool subsonicart_prune(const subsonic_client_t *client,const char *root) {
    char source[33];subsoniccache_source(client,source);
    char (*hashes)[33]=NULL;int used=0,capacity=0;
    bool ok=true;
    for(int offset=0;;) {
        char ids[50][128];int count=library_remote_art_ids(source,offset,ids,50);
        if(count<0){ok=false;break;}
        if(used+count>capacity) {
            int next=capacity ? capacity*2 : 64;
            if(next<used+count)next=used+count;
            void *grown=realloc(hashes,(size_t)next*sizeof(*hashes));
            if(!grown){ok=false;break;}hashes=grown;capacity=next;
        }
        for(int i=0;i<count;i++)md5_hex(ids[i],strlen(ids[i]),hashes[used++]);
        offset+=count;if(count<50)break;
    }
    if(!ok){free(hashes);return false;}
    if(used)qsort(hashes,(size_t)used,sizeof(*hashes),compare_hash);
    char directory[560];snprintf(directory,sizeof(directory),"%s/.local/subsonic-art",root);
    DIR *dir=opendir(directory);
    if(!dir){free(hashes);return errno==ENOENT;}
    struct dirent *entry;
    while((entry=readdir(dir))) {
        if(strncmp(entry->d_name,source,32) || entry->d_name[32]!='-')continue;
        char hash[33]="";
        bool published=strlen(entry->d_name)==69 && !strcmp(entry->d_name+65,".jpg");
        if(published)memcpy(hash,entry->d_name+33,32);
        if(published && used && bsearch(hash,hashes,(size_t)used,sizeof(*hashes),compare_hash))continue;
        char path[1024];int n=snprintf(path,sizeof(path),"%s/%s",directory,entry->d_name);
        struct stat st;
        if(n<0 || n>=(int)sizeof(path)){ok=false;continue;}
        if(lstat(path,&st)==0) {
            if(!S_ISREG(st.st_mode) || unlink(path)!=0)ok=false;
        }else if(errno!=ENOENT)ok=false;
    }
    closedir(dir);free(hashes);return ok;
}
