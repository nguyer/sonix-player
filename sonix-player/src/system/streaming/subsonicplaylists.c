#include "subsonicplaylists.h"
#include "subsonicplaylistapi.h"
#include "subsoniccache.h"
#include "src/system/library/library.h"
#include "src/system/library/libraryremote.h"
#include "src/system/core/md5.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static atomic_bool syncing;
bool subsonicplaylists_busy(void) { return atomic_load(&syncing); }
static unsigned revision(void) { return library_revision(LIBRARY_LIST_PLAYLIST); }
static bool fail(subsonic_client_t *c,const char *name,const char *why) {
	snprintf(c->error,sizeof(c->error),"%.85s: %.160s",name,why);return false;
}
static void hash_ids(const char (*ids)[128],int count,char out[33]) { md5_hex(count?(const void *)ids:(const void *)"",(size_t)count*128,out); }

static bool local_snapshot(subsonic_client_t *c,const library_playlist_link_t *link,
	char (**ids)[128],int *count,unsigned *rev,char hash[33]) {
	if(!library_playlist_remote_ids(link->source,link->local_name,ids,count,rev))
		return fail(c,link->local_name,"Contains tracks without IDs from this account, or the library is busy. No changes sent.");
	hash_ids((const char (*)[128])*ids,*count,hash);return true;
}

static void local_name(const char *desired,const char *id,const char *existing,char out[201]) {
	size_t n=strlen(desired);if(n>160)n=160;
	while(n && ((unsigned char)desired[n]&0xc0)==0x80)--n;
	memcpy(out,desired,n);out[n]=0;
	for(size_t i=0;i<n;++i) if(strchr("/\\:*?\"<>|\r\n",out[i]) || (i==0 && out[i]=='.')) out[i]='_';
	if(!*out) strcpy(out,"Navidrome playlist");
	library_playlist_link_t reserved;
	if((!existing || strcmp(out,existing)) && (library_playlist_exists(out) || library_playlist_link(out,&reserved))) {
		char hash[33],base[170];md5_hex(id,strlen(id),hash);strcpy(base,out);
		for(int suffix=0;suffix<1000;++suffix) {
			snprintf(out,201,"%.160s [Navidrome %.8s-%d]",base,hash,suffix);
			if((existing && !strcmp(out,existing)) || (!library_playlist_exists(out) && !library_playlist_link(out,&reserved))) return;
		}
	}
}

static void baseline(library_playlist_link_t *link,const subsonic_playlist_snapshot_t *remote,const char *hash) {
	strcpy(link->id,remote->info.id);strcpy(link->remote_name,remote->info.name);
	strcpy(link->base_local,link->local_name);strcpy(link->hash,hash);
}

static bool sync_one(subsonic_client_t *c,const subsonic_playlist_t *info,
	const library_playlist_link_t *saved,const char *source) {
	subsonic_playlist_snapshot_t remote;
	if(!subsonic_playlist_get(c,info->id,&remote)) return false;
	char remote_hash[33];hash_ids((const char (*)[128])remote.ids,remote.count,remote_hash);
	library_playlist_link_t link={0};
	bool ok=true;
	if(!saved) {
		strcpy(link.source,source);local_name(remote.info.name,info->id,NULL,link.local_name);
		baseline(&link,&remote,remote_hash);
		ok=library_playlist_remote_replace(&link,NULL,(const char (*)[128])remote.ids,remote.count,revision());
		if(!ok) fail(c,remote.info.name,"Cannot import playlist. Sync the song catalog first and wait for any library scan to finish.");
	} else {
		link=*saved;
		bool remote_changed=strcmp(remote_hash,link.hash) || strcmp(remote.info.name,link.remote_name);
		bool writable=!strcmp(remote.info.owner,c->username);
		if(!library_playlist_exists(link.local_name)) {
			unsigned rev=revision();
			if(remote_changed || !writable) ok=fail(c,link.local_name,"Deletion conflicts with the server. The server playlist was kept.");
			else {
				library_playlist_link_t current;
				ok=library_playlist_link(link.local_name,&current) && !strcmp(current.id,link.id) &&
					subsonic_playlist_delete(c,link.id) && library_playlist_link_drop(&link,false,rev);
			}
		} else {
			char (*ids)[128]=NULL,hash[33];int count=0;unsigned rev=0;
			ok=local_snapshot(c,&link,&ids,&count,&rev,hash);
			bool local_changed=ok && (strcmp(hash,link.hash) || strcmp(link.local_name,link.base_local));
			if(ok && local_changed && remote_changed) {
				// Identical edits or a retry after a successful write can converge.
				bool same_name=!strcmp(link.local_name,remote.info.name) || !strcmp(link.local_name,link.base_local);
				if(!strcmp(hash,remote_hash) && same_name) {
					if(!strcmp(link.local_name,link.base_local) && strcmp(remote.info.name,link.remote_name)) {
						char old[201];strcpy(old,link.local_name);
						local_name(remote.info.name,remote.info.id,old,link.local_name);
						baseline(&link,&remote,hash);
						ok=library_playlist_remote_replace(&link,old,(const char (*)[128])remote.ids,remote.count,rev);
					} else {baseline(&link,&remote,hash);ok=library_playlist_link_record(&link,rev);}
				} else ok=fail(c,link.local_name,"Changed both here and on the server. Both versions were kept; resolve the edits before syncing again.");
			} else if(ok && local_changed) {
				if(!writable) ok=fail(c,link.local_name,"This shared playlist is read-only. Local edits were kept.");
				else if(revision()!=rev) ok=false;
				else {
					char id[128];
					const char *name=strcmp(link.local_name,link.base_local)?link.local_name:remote.info.name;
					ok=subsonic_playlist_put(c,link.id,name,(const char (*)[128])ids,count,id);
					if(ok) {strcpy(remote.info.name,name);baseline(&link,&remote,hash);ok=library_playlist_link_record(&link,rev);}
				}
			} else if(ok && remote_changed) {
				char old[201];strcpy(old,link.local_name);
				if(strcmp(remote.info.name,link.remote_name)) local_name(remote.info.name,remote.info.id,old,link.local_name);
				baseline(&link,&remote,remote_hash);
				ok=library_playlist_remote_replace(&link,old,(const char (*)[128])remote.ids,remote.count,rev);
			}
			if(ok && !local_changed && !remote_changed)ok=library_playlist_link_record(&link,rev);
			free(ids);
		}
	}
	subsonic_playlist_free(&remote);
	if(!ok && !c->error[0]) fail(c,info->name,"Playlist changed while syncing or storage is unavailable. Try again.");
	return ok;
}

static int sync_playlists(subsonic_client_t *c,const atomic_bool *cancel,bool pending_only) {
	atomic_store(&syncing,true);c->error[0]=0;
	char source[33];subsoniccache_source(c,source);
	subsonic_playlist_t *remote=NULL;library_playlist_link_t *links=NULL;
	int n=subsonic_playlists(c,&remote), linked=library_playlist_links(source,&links), done=0;
	bool ok=n>=0 && linked>=0,all_ok=true;
	char first_error[256]="";
	for(int i=0;ok && i<n;++i) {
		if(atomic_load(cancel)){ok=false;break;}
		const library_playlist_link_t *found=NULL;
		for(int j=0;j<linked;++j) if(!strcmp(links[j].id,remote[i].id)){found=&links[j];break;}
		if(pending_only && (!found || !library_playlist_auto_link_pending(source,found->id)))continue;
		bool one=sync_one(c,&remote[i],found,source);
  if(!one && pending_only) {
   all_ok=false;
   if(!first_error[0])snprintf(first_error,sizeof(first_error),"%s",c->error);
   c->error[0]=0;
  } else {ok=one;if(ok)++done;}
	}
	for(int i=0;ok && i<linked;++i) {
		bool found=false;for(int j=0;j<n;++j) if(!strcmp(links[i].id,remote[j].id))found=true;
		if(found || (pending_only && !library_playlist_auto_link_pending(source,links[i].id))) continue;
		if(atomic_load(cancel)){ok=false;break;}
		unsigned rev=revision();
		if(library_playlist_exists(links[i].local_name)) {
			char (*ids)[128]=NULL,hash[33];int count=0;
			ok=local_snapshot(c,&links[i],&ids,&count,&rev,hash);free(ids);
			if(ok && (strcmp(hash,links[i].hash) || strcmp(links[i].local_name,links[i].base_local)))
				ok=fail(c,links[i].local_name,"Removed on the server but edited locally. The local playlist was kept.");
		}
		if(ok) ok=library_playlist_link_drop(&links[i],true,rev);
  if(!ok && pending_only) {
   all_ok=false;
   if(!first_error[0])snprintf(first_error,sizeof(first_error),"%s",c->error[0]?c->error:"Playlist changed during sync. Try again.");
   c->error[0]=0;ok=true;
  }
	}
	free(remote);free(links);atomic_store(&syncing,false);
 if(ok && !all_ok){ok=false;snprintf(c->error,sizeof(c->error),"%s",first_error);}
	if(!ok && !c->error[0]) fail(c,"Playlist sync","Cancelled, library busy, or playlists changed during sync. Completed playlists remain synchronized.");
	return ok?done:-1;
}

int subsonicplaylists_sync(subsonic_client_t *c,const atomic_bool *cancel) {return sync_playlists(c,cancel,false);}

int subsonicplaylists_sync_pending(subsonic_client_t *c,const atomic_bool *cancel) {return sync_playlists(c,cancel,true);}

bool subsonicplaylists_upload(subsonic_client_t *c,const char *name,const atomic_bool *cancel) {
	library_playlist_link_t link={0};
	if(library_playlist_link(name,&link)) {
		char source[33];subsoniccache_source(c,source);
		if(strcmp(source,link.source)) return fail(c,name,"Connect to this playlist's Navidrome account first.");
		return subsonicplaylists_sync(c,cancel)>=0;
	}
	atomic_store(&syncing,true);c->error[0]=0;
	subsoniccache_source(c,link.source);snprintf(link.local_name,sizeof(link.local_name),"%s",name);
	char (*ids)[128]=NULL,hash[33];int count=0;unsigned rev=0;
	bool ok=local_snapshot(c,&link,&ids,&count,&rev,hash);
	// Recover an earlier creation whose reply or local mapping write was lost.
	subsonic_playlist_t *existing=NULL;
	int n=ok?subsonic_playlists(c,&existing):-1;
	if(n<0)ok=false;
	int matched=0;
	for(int i=0;ok && i<n;++i) if(!strcmp(existing[i].owner,c->username) && !strcmp(existing[i].name,name)) {
		subsonic_playlist_snapshot_t p;
		if(!subsonic_playlist_get(c,existing[i].id,&p)){ok=false;break;}
		char h[33];hash_ids((const char (*)[128])p.ids,p.count,h);
		if(!strcmp(h,hash)){strcpy(link.id,p.info.id);++matched;}
		subsonic_playlist_free(&p);
	}
	free(existing);
	if(matched>1)ok=fail(c,name,"More than one identical server playlist exists. Sync from the server before uploading.");
	if(ok && (atomic_load(cancel) || revision()!=rev))ok=false;
	if(ok && !matched) ok=subsonic_playlist_put(c,NULL,name,(const char (*)[128])ids,count,link.id);
	if(ok) {
		strcpy(link.base_local,name);strcpy(link.remote_name,name);strcpy(link.hash,hash);
		ok=library_playlist_link_record(&link,rev);
	}
	free(ids);atomic_store(&syncing,false);
	if(!ok && !c->error[0])fail(c,name,"Upload interrupted. A server copy may exist; refresh or retry to recover it. Local playlist was kept.");
	return ok;
}
