#include "src/system/core/lang.h"
#include "subsonicplaylistapi.h"
#include "subsonicinternal.h"
#include "src/system/net/http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool text(const json_doc_t *d, int obj, const char *key, char *out, size_t size) {
	int t = json_get(d, obj, key);
	return t >= 0 && d->toks[t].type == JSON_STRING && d->toks[t].end - d->toks[t].start < (int)size &&
		json_str(d, t, out, size) && *out;
}
static bool info(const json_doc_t *d, int obj, subsonic_playlist_t *p) {
	memset(p, 0, sizeof(*p));
	return text(d,obj,"id",p->id,sizeof(p->id)) && text(d,obj,"name",p->name,sizeof(p->name)) &&
		text(d,obj,"owner",p->owner,sizeof(p->owner));
}
static bool bad(subsonic_client_t *c) { snprintf(c->error,sizeof(c->error),"%s",tr("navidrome_invalid_or_oversized_server_playlist")); return false; }

int subsonic_playlists(subsonic_client_t *c, subsonic_playlist_t **out) {
	*out = NULL;
	char *body; json_doc_t d; int root;
	if (!subsonic_api_call(c,"getPlaylists","",&body,&d,&root)) return -1;
	int group = json_get(&d,root,"playlists"), list = json_get(&d,group,"playlist");
	int n = json_len(&d,list);
	bool ok = group >= 0 && d.toks[group].type == JSON_OBJECT && n <= 1000 &&
		(list < 0 || d.toks[list].type == JSON_ARRAY);
	subsonic_playlist_t *items = ok && n ? calloc((size_t)n,sizeof(*items)) : NULL;
	if (n && !items) ok = false;
	for (int i=0; ok && i<n; ++i) {
		ok = info(&d,json_at(&d,list,i),&items[i]);
		for (int j=0; ok && j<i; ++j) if (!strcmp(items[i].id,items[j].id)) ok = false;
	}
	json_free(&d); free(body);
	if (!ok) { free(items); bad(c); return -1; }
	*out = items; return n;
}

bool subsonic_playlist_get(subsonic_client_t *c, const char *id, subsonic_playlist_snapshot_t *out) {
	memset(out,0,sizeof(*out));
	char encoded[384], params[400], *body; json_doc_t d; int root;
	http_url_encode(id,encoded,sizeof(encoded)); snprintf(params,sizeof(params),"&id=%s",encoded);
	if (!subsonic_api_call(c,"getPlaylist",params,&body,&d,&root)) return false;
	int p = json_get(&d,root,"playlist"), entries = json_get(&d,p,"entry");
	int n = json_len(&d,entries);
	bool ok = info(&d,p,&out->info) && !strcmp(id,out->info.id) && n <= 10000 &&
		(entries < 0 || d.toks[entries].type == JSON_ARRAY) && json_obj_long(&d,p,"songCount",n) == n;
	if (ok && n) { out->ids=calloc((size_t)n,128); if (!out->ids) ok=false; }
	for (int i=0; ok && i<n; ++i) ok=text(&d,json_at(&d,entries,i),"id",out->ids[i],128);
	json_free(&d); free(body);
	if (!ok) { subsonic_playlist_free(out); return bad(c); }
	out->count=n; return true;
}

void subsonic_playlist_free(subsonic_playlist_snapshot_t *p) { free(p->ids); memset(p,0,sizeof(*p)); }

bool subsonic_playlist_put(subsonic_client_t *c, const char *id, const char *name,
	const char (*ids)[128], int count, char out_id[128]) {
	if (count<0 || count>10000 || !name || strlen(name)>255 || (id && strlen(id)>127)) return bad(c);
	size_t size=(size_t)count*400+1200;
	char *params=malloc(size);
	if (!params) return bad(c);
	char encoded[768];
	http_url_encode(id && *id ? id : name,encoded,sizeof(encoded));
	size_t at=(size_t)snprintf(params,size,"&%s=%s",id && *id ? "playlistId" : "name",encoded);
	for (int i=0; i<count; ++i) {
		if (!ids[i][0] || strnlen(ids[i],128)==128) { free(params); return bad(c); }
		http_url_encode(ids[i],encoded,sizeof(encoded));
		at+=(size_t)snprintf(params+at,size-at,"&songId=%s",encoded);
	}
	char *body; json_doc_t d; int root;
	bool ok=subsonic_api_call(c,"createPlaylist",params,&body,&d,&root);
	free(params);
	if (!ok) return false;
	ok=text(&d,json_get(&d,root,"playlist"),"id",out_id,128);
	json_free(&d); free(body);
	if (!ok) return bad(c);
	if (id && *id) {
		char encoded_id[384], encoded_name[768], rename[1200];
		http_url_encode(id,encoded_id,sizeof(encoded_id)); http_url_encode(name,encoded_name,sizeof(encoded_name));
		snprintf(rename,sizeof(rename),"&playlistId=%s&name=%s",encoded_id,encoded_name);
		ok=subsonic_api_call(c,"updatePlaylist",rename,&body,&d,&root);
		if (ok) { json_free(&d); free(body); }
	}
	return ok;
}

bool subsonic_playlist_delete(subsonic_client_t *c, const char *id) {
	char encoded[384], params[410], *body; json_doc_t d; int root;
	http_url_encode(id,encoded,sizeof(encoded)); snprintf(params,sizeof(params),"&id=%s",encoded);
	if (!subsonic_api_call(c,"deletePlaylist",params,&body,&d,&root)) return false;
	json_free(&d); free(body); return true;
}
