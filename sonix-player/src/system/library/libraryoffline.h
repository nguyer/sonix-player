#ifndef LIBRARYOFFLINE_H
#define LIBRARYOFFLINE_H
#include "libraryremote.h"
#include <stdint.h>
typedef enum { OFFLINE_OFF, OFFLINE_SELECTIVE, OFFLINE_MIRROR } offline_mode_t;
typedef enum { OFFLINE_SONG=1, OFFLINE_ALBUM, OFFLINE_ARTIST, OFFLINE_PLAYLIST,
 OFFLINE_FAVORITES, OFFLINE_RECENT, OFFLINE_PLAYED, OFFLINE_EXCLUDE } offline_rule_t;
typedef struct {
 int mode, automatic, charging_only, cleanup, interval_minutes, auto_cache;
 int64_t max_bytes, reserve_bytes, last_run;
} offline_options_t;
typedef struct { char value[256], label[256]; bool selected; } offline_choice_t;
typedef struct { char id[128], path[512]; int64_t bytes, stamp, remote_bytes; int wanted, status; bool catalog_present; } offline_file_t;
// Atomically change a group selection and individual exclusions.
bool library_offline_select(const char *source,int kind,const char *value,const char (*ids)[128],int count,bool selected);
bool library_offline_clear(const char *source);
bool library_offline_wanted(const char *path);
bool library_offline_options(const char *source,offline_options_t *out);
bool library_offline_save_options(const char *source,const offline_options_t *options);
bool library_offline_rule(const char *source,int kind,const char *value,bool selected);
bool library_offline_has_rule(const char *source,int kind,const char *value);
int library_offline_choices(const char *source,int kind,const char *filter,int offset,offline_choice_t *out,int max);
bool library_offline_plan(const char *source,bool mirror,const char (*extra_ids)[128],int extra_count);
int library_offline_files(const char *source,int offset,offline_file_t *out,int max);
bool library_offline_note(const char *source,const char *id,int64_t bytes,int64_t stamp,int status);
bool library_offline_forget(const char *source,const char *id);
bool library_offline_path_protected(const char *path);
bool library_offline_complete(const char *source,int64_t when);
int64_t library_offline_bytes(const char *source);
#endif
