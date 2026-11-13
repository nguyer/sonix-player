#include "speaker_output.h"
#include "speaker_share.h"
#include "src/system/decode/growfile.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// The UI writes intent, never waits for HTTP, and never shares a sonos_t with
// a networking thread. Revisions discard stale replies after a skip/switch.
enum { LOAD = 1, TRANSPORT = 2, SEEK = 4, VOLUME = 8, TARGET = 16, VOLUME_DELTA = 32 };
typedef struct {
    bool active;
    sonos_t sonos;
    char room[SONOS_ID_MAX], name[128], file[512];
    audio_status_t status;
    double position, duration;
    int volume, volume_target, volume_delta;
    unsigned serial, commands;
} output_t;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static output_t desired = {.volume = 0};
static bool started, completed;
static char error[192];
static double position_tick, volume_tick;
static int confirmed_volume;
static unsigned group_volume_revision;
static bool group_volume_inflight;
static void (*volume_preview_cb)(const char *,int,bool,double);
void speaker_output_set_volume_preview_cb(void (*cb)(const char *,int,bool,double)) {
    pthread_mutex_lock(&mutex);volume_preview_cb=cb;pthread_mutex_unlock(&mutex);
}
bool speaker_output_group_volume_pending(void) {
    pthread_mutex_lock(&mutex);
    bool pending=group_volume_inflight || (desired.commands & (VOLUME|VOLUME_DELTA));
    pthread_mutex_unlock(&mutex);return pending;
}
static double monotonic_seconds(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return t.tv_sec + t.tv_nsec / 1000000000.0;
}
// Speaker positions have one-second resolution and arrive less often than UI
// paints. Extrapolate one shared position rather than repeatedly exposing an
// old whole-second sample. Stop extrapolating if feedback is overdue.
static double position_now(double now) {
    double position=desired.position;
    if (desired.status==AUDIO_STATUS_PLAYING && position_tick>0) {
        double elapsed=now-position_tick;
        if(elapsed>3)elapsed=3;
        if(elapsed>0)position+=elapsed;
    }
    if(desired.duration>0 && position>desired.duration)position=desired.duration;
    return position;
}
static void (*changed_cb)(void), (*completion_cb)(void);
void speaker_output_set_completion_cb(void (*callback)(void)) {
    pthread_mutex_lock(&mutex); completion_cb=callback; pthread_mutex_unlock(&mutex);
}
void speaker_output_set_changed_cb(void (*callback)(void)) {
    pthread_mutex_lock(&mutex); changed_cb=callback; pthread_mutex_unlock(&mutex);
}

static void signal_command(unsigned command) {
    desired.commands |= command; desired.serial++; completed = false;
    pthread_cond_signal(&wake);
}
static struct timespec deadline(int ms) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    t.tv_nsec += (ms % 1000) * 1000000L; t.tv_sec += ms / 1000 + t.tv_nsec / 1000000000L;
    t.tv_nsec %= 1000000000L; return t;
}
static void requeue(unsigned commands) {
    pthread_mutex_lock(&mutex); desired.commands |= commands;
    pthread_cond_signal(&wake); pthread_mutex_unlock(&mutex);
}
static bool current(unsigned serial) {
    pthread_mutex_lock(&mutex); bool same = desired.serial == serial;
    pthread_mutex_unlock(&mutex); return same;
}
static void *worker(void *unused) {
    (void)unused;
    output_t previous = {0};
    char uri[256] = "";
    bool owned = false, saw_playing = false, foreign = false;
    double last_position = 0, last_duration = 0;
    for (;;) {
        pthread_mutex_lock(&mutex);
        if (!desired.commands) {
            if (!desired.active) {
                while (!desired.commands) pthread_cond_wait(&wake, &mutex);
            } else {
                struct timespec until = deadline(1000);
                pthread_cond_timedwait(&wake, &mutex, &until);
            }
        }
        output_t job = desired; desired.commands = 0; desired.volume_delta=0;
        group_volume_inflight=job.active && (job.commands & (VOLUME|VOLUME_DELTA));
        pthread_mutex_unlock(&mutex);
        bool target_changed = (job.commands & TARGET) || job.active != previous.active;
        if (target_changed) {
            if (previous.active && owned) {
                sonos_playback_t live;
                // Another controller may have replaced the stream since our
                // last observation. Switching output must not stop its music.
                sonos_t stop_context=job.active ? previous.sonos : job.sonos;
                const char *stop_room=job.active ? previous.room : job.room;
                if (sonos_get_playback(&stop_context,stop_room,&live) && !strcmp(live.uri,uri))
                    sonos_transport(&stop_context,stop_room,"Stop");
            }
            speaker_share_revoke(); uri[0] = 0; owned = saw_playing = foreign = false;
        }
        if (!job.active) { previous = job; continue; }
        bool ok = true;
        sonos_t controller = job.sonos;
        // Volume keys remain responsive while a track finishes downloading.
        if (job.commands & (VOLUME|VOLUME_DELTA)) {
            if(job.commands & VOLUME)ok=sonos_group_volume(&controller,job.room,job.volume_target,true);
            if(ok && (job.commands & VOLUME_DELTA))ok=sonos_group_volume(&controller,job.room,job.volume_delta,false);
            pthread_mutex_lock(&mutex);group_volume_revision++;group_volume_inflight=false;pthread_mutex_unlock(&mutex);
            // Relative adjustments are consumed exactly once, even if a newer
            // command arrives while HTTP is in flight. Never replay a delta.
            job.commands &= ~(VOLUME|VOLUME_DELTA);
        }
        if (ok && (job.commands & LOAD) && job.file[0]) {
            // Sonos requires a stable Content-Length. Wait off-thread until a
            // streamed/cache file is complete; skips cancel this wait.
            if (owned && growfile_is_growing(job.file)) {
                sonos_transport(&controller,job.room,"Stop");
                speaker_share_revoke(); uri[0]=0; owned=saw_playing=foreign=false;
            }
            while (growfile_is_growing(job.file) && current(job.serial)) {
                pthread_mutex_lock(&mutex);
                if (desired.serial == job.serial) {
                    struct timespec until = deadline(100);
                    pthread_cond_timedwait(&wake, &mutex, &until);
                }
                pthread_mutex_unlock(&mutex);
            }
            if (!current(job.serial)) { requeue(job.commands); previous = job; continue; }
            char ip[40];
            ok = sonos_refresh(&controller);
            const sonos_room_t *room = NULL, *coordinator = NULL;
            if (ok) {
                for (int i=0;i<controller.count;i++) if (!strcmp(controller.rooms[i].id, job.room)) room=&controller.rooms[i];
                if (room) for (int i=0;i<controller.count;i++)
                    if (!strcmp(controller.rooms[i].id,room->coordinator)) coordinator=&controller.rooms[i];
                ok = coordinator && speaker_share_address(coordinator->ip, ip, sizeof(ip));
                if (!ok) snprintf(controller.error,sizeof(controller.error),"Selected speaker is unavailable");
            }
            if (ok) {
                ok = speaker_share_file(job.file,ip,uri,sizeof(uri));
                if (!ok) snprintf(controller.error,sizeof(controller.error),"Cannot share this track with Sonos");
            }
            if (ok && current(job.serial)) {
                ok = sonos_load_uri(&controller,job.room,uri,false);
                owned = ok; foreign = false; saw_playing = false; last_position = last_duration = 0;
                if (ok && job.position > 0) ok = sonos_seek(&controller,job.room,job.position);
                if (ok && job.status == AUDIO_STATUS_PLAYING)
                    ok = sonos_transport(&controller,job.room,"Play");
            } else if (ok) {
                // The next iteration will process the replacement intent.
                requeue(job.commands); previous = job; continue;
            }
        } else if (ok) {
            if (job.commands & TRANSPORT) {
                ok = sonos_transport(&controller,job.room,job.status == AUDIO_STATUS_PLAYING ? "Play" :
                    job.status == AUDIO_STATUS_PAUSED ? "Pause" : "Stop");
                if (job.status == AUDIO_STATUS_STOPPED) saw_playing = false;
            }
            if (ok && (job.commands & SEEK)) ok = sonos_seek(&controller,job.room,job.position);
        }
        sonos_playback_t feedback = {0};
        if (ok) ok = sonos_get_playback(&controller,job.room,&feedback);
        if (ok && !strcmp(feedback.state,"TRANSITIONING")) {
            if (!current(job.serial)) requeue(job.commands);
            previous = job; continue; // retain the pending status through buffering
        }
        bool ended = false;
        pthread_mutex_lock(&mutex);
        if (desired.serial == job.serial && desired.active) {
            if (!ok) {
                desired.position=position_now(monotonic_seconds());
                position_tick=0;
                desired.status = AUDIO_STATUS_STOPPED;
                saw_playing = false;
                if (strcmp(error,controller.error)) fprintf(stderr,"Sonos output: %s\n",controller.error);
                snprintf(error,sizeof(error),"%s",controller.error);
            } else {
                error[0] = 0;
                audio_status_t status = !strcmp(feedback.state,"PLAYING") ? AUDIO_STATUS_PLAYING :
                    !strcmp(feedback.state,"PAUSED_PLAYBACK") ? AUDIO_STATUS_PAUSED : AUDIO_STATUS_STOPPED;
                bool same_uri = owned && !strcmp(uri,feedback.uri);
                if (same_uri && !saw_playing && status == AUDIO_STATUS_STOPPED &&
                    job.status == AUDIO_STATUS_PAUSED) status = AUDIO_STATUS_PAUSED;
                // An explicit Stop, disconnect or another controller's track
                // must never advance this device's queue.
                if (same_uri && saw_playing && status == AUDIO_STATUS_STOPPED &&
                    !(job.commands & (LOAD|TRANSPORT)) && last_duration > 0 &&
                    last_position + 3 >= last_duration) { completed = true; ended = true; }
                if (status == AUDIO_STATUS_STOPPED) saw_playing = false;
                if (same_uri && status == AUDIO_STATUS_PLAYING) saw_playing = true;
                double now=monotonic_seconds(), position=feedback.position;
                if(status==AUDIO_STATUS_PLAYING && feedback.position_sample>0)
                    position+=now-feedback.position_sample;
                // Quantized/late reports must not rewind the moving bar.
                // A genuine backward seek (or explicit local seek/load) resets it.
                double predicted=position_now(now);
                if(same_uri && !(job.commands & (LOAD|SEEK|TRANSPORT)) &&
                    desired.status==AUDIO_STATUS_PLAYING && status==AUDIO_STATUS_PLAYING &&
                    position<predicted && predicted-position<2.0) position=predicted;
                desired.status = status;
                desired.position = position; desired.duration = feedback.duration;
                position_tick=now;
                desired.volume = feedback.volume; confirmed_volume=feedback.volume; volume_tick=now;
                last_position = feedback.position;
                if (feedback.duration > 0) last_duration = feedback.duration;
                // Don't present an old local title for a track another
                // controller substituted; resume/next still use our queue.
                if (foreign || (!same_uri && owned)) {
                    owned = false; foreign = true; desired.status = AUDIO_STATUS_STOPPED;
                    snprintf(error,sizeof(error),"Speaker playback changed in another controller");
                }
            }
        } else {
            desired.commands |= job.commands;
            pthread_cond_signal(&wake);
        }
        void (*notify)(void) = changed_cb;
        void (*finish)(void) = ended ? completion_cb : NULL;
        pthread_mutex_unlock(&mutex);
        if (notify) notify();
        if (finish) finish();
        previous = job;
    }
    return NULL;
}
bool speaker_output_select(const sonos_t *rooms, const char *id, const char *path, double position, bool playing) {
    const sonos_room_t *room = NULL;
    if (!rooms || !id) return false;
    for (int i=0;i<rooms->count;i++) if (!strcmp(rooms->rooms[i].id,id)) room=&rooms->rooms[i];
    if (!room) return false;
    pthread_mutex_lock(&mutex);
    if (!started) {
        pthread_t thread;
        if (pthread_create(&thread,NULL,worker,NULL)) { pthread_mutex_unlock(&mutex); return false; }
        pthread_detach(thread); started = true;
    }
    desired.active = true; desired.sonos = *rooms;
    desired.commands &= ~(VOLUME|VOLUME_DELTA); desired.volume_delta=0;
    snprintf(desired.room,sizeof(desired.room),"%s",id);
    snprintf(desired.name,sizeof(desired.name),"%s",room->name);
    snprintf(desired.file,sizeof(desired.file),"%s",path ? path : "");
    desired.position = position > 0 ? position : 0; desired.duration = 0; desired.volume = 0;
    position_tick=0; volume_tick=0;
    desired.status = playing ? AUDIO_STATUS_PLAYING : AUDIO_STATUS_PAUSED;
    error[0]=0;
    signal_command(TARGET | (desired.file[0] ? LOAD : 0));
    pthread_mutex_unlock(&mutex); return true;
}
void speaker_output_local(void) {
    pthread_mutex_lock(&mutex); desired.active=false; signal_command(TARGET);
    pthread_mutex_unlock(&mutex);
}
bool speaker_output_active(void) {
    pthread_mutex_lock(&mutex); bool active=desired.active; pthread_mutex_unlock(&mutex); return active;
}
void speaker_output_room(char *out,size_t cap) {
    pthread_mutex_lock(&mutex);
    snprintf(out,cap,"%s",desired.active ? desired.room : "");
    pthread_mutex_unlock(&mutex);
}
bool speaker_output_follow(const sonos_t *rooms,const char *id) {
    const sonos_room_t *room=NULL;
    for(int i=0;i<rooms->count;i++)if(!strcmp(rooms->rooms[i].id,id))room=&rooms->rooms[i];
    if(!room)return false;
    pthread_mutex_lock(&mutex);
    bool active=desired.active;
    if(active) {
        desired.sonos=*rooms; volume_tick=0;
        snprintf(desired.room,sizeof(desired.room),"%s",id);
        snprintf(desired.name,sizeof(desired.name),"%s",room->name);
        // Coordinator changes must invalidate replies, but not republish audio.
        signal_command(0);
    }
    pthread_mutex_unlock(&mutex);return active;
}
void speaker_output_name(char *out,size_t cap) {
    pthread_mutex_lock(&mutex); snprintf(out,cap,"%s",desired.active ? desired.name : "This device"); pthread_mutex_unlock(&mutex);
}
void speaker_output_error(char *out,size_t cap) {
    pthread_mutex_lock(&mutex); snprintf(out,cap,"%s",error); pthread_mutex_unlock(&mutex);
}
void speaker_output_play(const char *path,double position,bool paused) {
    pthread_mutex_lock(&mutex);
    snprintf(desired.file,sizeof(desired.file),"%s",path);
    desired.position=position > 0 ? position : 0; desired.duration=0; position_tick=0;
    desired.status=paused ? AUDIO_STATUS_PAUSED : AUDIO_STATUS_PLAYING;
    error[0]=0; signal_command(LOAD); pthread_mutex_unlock(&mutex);
}
void speaker_output_transport(audio_status_t status) {
    pthread_mutex_lock(&mutex); desired.position=position_now(monotonic_seconds());
    position_tick=monotonic_seconds(); desired.status=status; signal_command(TRANSPORT); pthread_mutex_unlock(&mutex);
}
void speaker_output_seek(double position) {
    if (!(position >= 0 && position < 36000000)) return;
    pthread_mutex_lock(&mutex); desired.position=position; position_tick=monotonic_seconds(); signal_command(SEEK); pthread_mutex_unlock(&mutex);
}
audio_status_t speaker_output_status(void) {
    pthread_mutex_lock(&mutex); audio_status_t status=desired.status; pthread_mutex_unlock(&mutex); return status;
}
void speaker_output_file(char *out,size_t cap) {
    pthread_mutex_lock(&mutex); snprintf(out,cap,"%s",desired.file); pthread_mutex_unlock(&mutex);
}
void speaker_output_progress(double *position,double *duration) {
    pthread_mutex_lock(&mutex); if(position)*position=position_now(monotonic_seconds()); if(duration)*duration=desired.duration; pthread_mutex_unlock(&mutex);
}
bool speaker_output_take_completion(void) {
    pthread_mutex_lock(&mutex); bool result=completed; completed=false; pthread_mutex_unlock(&mutex); return result;
}
int speaker_output_volume(void) {
    pthread_mutex_lock(&mutex); int value=desired.volume; pthread_mutex_unlock(&mutex); return value;
}
bool speaker_output_volume_feedback(char *room,size_t cap,int *percent,double *stamp) {
    pthread_mutex_lock(&mutex);
    bool valid=desired.active && volume_tick>0;
    if(valid) {
        snprintf(room,cap,"%s",desired.room);*percent=confirmed_volume;*stamp=volume_tick;
    }
    pthread_mutex_unlock(&mutex);return valid;
}
void speaker_output_set_volume(int percent) {
    pthread_mutex_lock(&mutex); desired.volume=percent < 0 ? 0 : percent > 100 ? 100 : percent;
    desired.volume_target=desired.volume;desired.volume_delta=0;desired.commands &= ~VOLUME_DELTA;
    char room[SONOS_ID_MAX];snprintf(room,sizeof(room),"%s",desired.room);
    void (*preview)(const char *,int,bool,double)=volume_preview_cb;
    double stamp=monotonic_seconds();
    int target=desired.volume_target;
    signal_command(VOLUME); pthread_mutex_unlock(&mutex);
    if(preview)preview(room,target,true,stamp);
}
void speaker_output_change_volume(int delta) {
    if(delta>100)delta=100;
    if(delta< -100)delta= -100;
    pthread_mutex_lock(&mutex);
    int display=desired.volume+delta;
    desired.volume=display<0 ? 0 : display>100 ? 100 : display;
    desired.volume_delta+=delta;
    if(desired.volume_delta>10000)desired.volume_delta=10000;
    if(desired.volume_delta< -10000)desired.volume_delta= -10000;
    char room[SONOS_ID_MAX];snprintf(room,sizeof(room),"%s",desired.room);
    void (*preview)(const char *,int,bool,double)=volume_preview_cb;
    double stamp=monotonic_seconds();
    signal_command(VOLUME_DELTA);pthread_mutex_unlock(&mutex);
    if(preview)preview(room,delta,false,stamp);
}
unsigned speaker_output_group_volume_revision(void) {
    pthread_mutex_lock(&mutex);unsigned revision=group_volume_revision;pthread_mutex_unlock(&mutex);return revision;
}
