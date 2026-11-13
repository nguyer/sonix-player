#include "src/system/core/lang.h"
#include "subsoniccleanup.h"
#include "subsoniccache.h"
#include "streamturn.h"
#include "src/system/library/libraryremote.h"
#include "src/system/library/library.h"
#include "src/system/library/playlists.h"
#include "src/gui/nowplaying/cover.h"
#include "src/system/audio/waveform.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static bool remove_regular(const char *path) {
    struct stat st;
    if(lstat(path,&st)!=0)return errno==ENOENT;
    return S_ISREG(st.st_mode) && unlink(path)==0;
}
static bool remove_files(subsonic_client_t *c,const char *root,const char *source) {
    for(int offset=0;;) {
        library_remote_removed_t rows[32];int n=library_remote_files(source,offset,rows,32);
        if(n<0)return false;
        for(int i=0;i<n;i++)if(!subsoniccache_remove_owned(c,root,rows[i].path,rows[i].id))return false;
        offset+=n;if(n<32)break;
    }
    // Include downloads left outside the current catalog; identity + expected
    // deterministic path prove ownership, never just a matching filename.
    char directory[560];snprintf(directory,sizeof(directory),"%s/.local/subsonic-cache",root);
    DIR *dir=opendir(directory);if(!dir)return errno==ENOENT;
    bool ok=true;struct dirent *entry;
    while((entry=readdir(dir))) {
        size_t n=strlen(entry->d_name);
        if(n<=9 || strcmp(entry->d_name+n-9,".identity"))continue;
        char path[512];
        int size=snprintf(path,sizeof(path),"%s/%.*s",directory,(int)(n-9),entry->d_name);
        if(size<0 || size>=(int)sizeof(path)){ok=false;continue;}
        subsonic_identity_t identity;
        if(subsoniccache_identity(path,&identity) && !strcmp(identity.server,c->base_url) &&
           !strcmp(identity.username,c->username) &&
           !subsoniccache_remove_owned(c,root,path,identity.song_id))ok=false;
    }
    closedir(dir);return ok;
}
static bool remove_art(const char *root,const char *source) {
    char directory[560];snprintf(directory,sizeof(directory),"%s/.local/subsonic-art",root);
    DIR *dir=opendir(directory);if(!dir)return errno==ENOENT;
    bool ok=true;struct dirent *entry;
    while((entry=readdir(dir))) {
        // Artwork is namespaced by the full account fingerprint. Includes
        // abandoned atomic-write temporary files in that same namespace.
        if(strncmp(entry->d_name,source,32) || entry->d_name[32]!='-')continue;
        char path[1024];int n=snprintf(path,sizeof(path),"%s/%s",directory,entry->d_name);
        if(n<0 || n>=(int)sizeof(path) || !remove_regular(path))ok=false;
    }
    closedir(dir);return ok;
}
bool subsoniccleanup_connection(subsonic_client_t *c,const char *root) {
    if(!c)return false;
    c->error[0]=0;
    if(!root || !*root || library_scan_running()){
        snprintf(c->error,sizeof(c->error),"%s",tr("navidrome_music_storage_is_unavailable_or_the_library_is_scanning"));return false;
    }
    char source[33];subsoniccache_source(c,source);
    streamturn_acquire();
    bool ok=remove_files(c,root,source);
    library_playlist_link_t *links=NULL;
    int count=ok ? library_playlist_links(source,&links) : -1;
    if(count<0)ok=false;
    for(int i=0;ok && i<count;i++)ok=playlists_remove_backup(links[i].local_name);
    free(links);
    if(ok)ok=remove_art(root,source);
    if(ok)ok=cover_clear_music_cache() && waveform_clear_cache();
    if(ok)ok=library_remote_forget(source);
    streamturn_release();
    if(!ok)snprintf(c->error,sizeof(c->error),"%s",tr("navidrome_connection_cleanup_could_not_finish_check_storage_then_try_remove"));
    return ok;
}
