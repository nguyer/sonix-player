#ifdef FULL_APP_TEST
#define sonos_refresh fake_sonos_refresh
#define sonos_load_uri fake_sonos_load_uri
#define sonos_transport fake_sonos_transport
#define sonos_seek fake_sonos_seek
#define sonos_volume fake_sonos_volume
#define sonos_group_volume fake_sonos_group_volume
#define sonos_get_playback fake_sonos_get_playback
#define growfile_is_growing fake_growfile_is_growing
#define speaker_share_address fake_speaker_share_address
#define speaker_share_file fake_speaker_share_file
#define speaker_share_revoke fake_speaker_share_revoke
#include "src/system/playback/device_state.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/core/config.h"
#endif
// Real output worker, with deterministic speaker and download stand-ins.
#include "src/system/remote/speaker_output.c"
#include <assert.h>
#include <unistd.h>
static pthread_mutex_t fake_lock=PTHREAD_MUTEX_INITIALIZER;
static sonos_playback_t remote={.state="STOPPED",.volume=23};
static bool online=true, growing, sharing, block_load, load_entered;
static unsigned loads, plays, stops, observations, completions;
static void finished(void) { pthread_mutex_lock(&fake_lock); completions++; pthread_mutex_unlock(&fake_lock); }
static char loaded[512], stop_room[80];
static void pause_ms(int ms) { usleep(ms*1000); }
bool growfile_is_growing(const char *path) {
    pthread_mutex_lock(&fake_lock); bool value=growing && strstr(path,"growing"); pthread_mutex_unlock(&fake_lock); return value;
}
bool speaker_share_address(const char *ip,char *out,size_t cap) { (void)ip; snprintf(out,cap,"192.168.1.10"); return true; }
bool speaker_share_file(const char *path,const char *ip,char *url,size_t cap) {
    (void)ip; pthread_mutex_lock(&fake_lock);
    snprintf(loaded,sizeof(loaded),"%s",path); sharing=true;
    snprintf(url,cap,"http://192.168.1.10/%s",strrchr(path,'/')+1);
    pthread_mutex_unlock(&fake_lock); return true;
}
void speaker_share_revoke(void) { pthread_mutex_lock(&fake_lock); sharing=false; pthread_mutex_unlock(&fake_lock); }
bool sonos_refresh(sonos_t *s) {
    pthread_mutex_lock(&fake_lock); bool ok=online; pthread_mutex_unlock(&fake_lock);
    if(!ok)snprintf(s->error,sizeof(s->error),"Speaker disconnected");
    return ok;
}
bool sonos_load_uri(sonos_t *s,const char *room,const char *uri,bool play) {
    (void)s; (void)room; assert(!play);
    pthread_mutex_lock(&fake_lock); load_entered=true; pthread_mutex_unlock(&fake_lock);
    for(;;) { pthread_mutex_lock(&fake_lock); bool wait=block_load; pthread_mutex_unlock(&fake_lock); if(!wait)break; pause_ms(2); }
    pthread_mutex_lock(&fake_lock); loads++;
    snprintf(remote.uri,sizeof(remote.uri),"%s",uri); snprintf(remote.state,sizeof(remote.state),"STOPPED");
    remote.position=0;remote.duration=100; pthread_mutex_unlock(&fake_lock); return true;
}
bool sonos_transport(sonos_t *s,const char *room,const char *action) {
    (void)s; pthread_mutex_lock(&fake_lock);
    if(!strcmp(action,"Play")) { snprintf(remote.state,sizeof(remote.state),"PLAYING"); plays++; }
    if(!strcmp(action,"Pause"))snprintf(remote.state,sizeof(remote.state),"PAUSED_PLAYBACK");
    if(!strcmp(action,"Stop")) { snprintf(remote.state,sizeof(remote.state),"STOPPED"); stops++; snprintf(stop_room,sizeof(stop_room),"%s",room); }
    pthread_mutex_unlock(&fake_lock); return true;
}
bool sonos_seek(sonos_t *s,const char *id,double position) { (void)s;(void)id;pthread_mutex_lock(&fake_lock);remote.position=position;pthread_mutex_unlock(&fake_lock);return true; }
bool sonos_volume(sonos_t *s,const char *id,int volume) { (void)s;(void)id;pthread_mutex_lock(&fake_lock);remote.volume=volume;pthread_mutex_unlock(&fake_lock);return true; }
bool sonos_group_volume(sonos_t *s,const char *id,int value,bool absolute) {
    (void)s;(void)id;pthread_mutex_lock(&fake_lock);
    int next=absolute ? value : remote.volume+value;
    remote.volume=next<0 ? 0 : next>100 ? 100 : next;
    pthread_mutex_unlock(&fake_lock);return true;
}
bool sonos_get_playback(sonos_t *s,const char *id,sonos_playback_t *out) {
    (void)id; if(!sonos_refresh(s))return false;
    pthread_mutex_lock(&fake_lock); *out=remote; observations++; pthread_mutex_unlock(&fake_lock);return true;
}
static void kick(void) { pthread_mutex_lock(&mutex);pthread_cond_signal(&wake);pthread_mutex_unlock(&mutex); }
static void wait_state(audio_status_t status) {
    for(int n=0;n<1500;n++) {
        pthread_mutex_lock(&fake_lock);
        bool matches=(status==AUDIO_STATUS_PLAYING&&!strcmp(remote.state,"PLAYING")) ||
            (status==AUDIO_STATUS_PAUSED&&(!strcmp(remote.state,"PAUSED_PLAYBACK")||!strcmp(remote.state,"STOPPED"))) ||
            (status==AUDIO_STATUS_STOPPED&&!strcmp(remote.state,"STOPPED"));
        pthread_mutex_unlock(&fake_lock);
        if(matches && speaker_output_status()==status)return;
        pause_ms(2);
    }
    assert(!"timed out waiting for speaker state");
}
static void observe(void) {
    pthread_mutex_lock(&fake_lock);unsigned before=observations;pthread_mutex_unlock(&fake_lock);
    kick();
    for(int n=0;n<1500;n++) {
        pthread_mutex_lock(&fake_lock);bool done=observations>before;pthread_mutex_unlock(&fake_lock);
        if(done) { pause_ms(10);return; } pause_ms(2);
    }
    assert(!"timed out reading speaker");
}
static void wait_loaded(const char *path) {
    for(int n=0;n<1500;n++) {
        pthread_mutex_lock(&fake_lock);bool match=!strcmp(loaded,path);pthread_mutex_unlock(&fake_lock);
        if(match) { pause_ms(20);return; } pause_ms(2);
    }
    assert(!"timed out loading track");
}
int main(int argc,char **argv) {
#ifndef FULL_APP_TEST
    (void)argc;(void)argv;
#endif
    sonos_t rooms={.count=2,.seed_ip="192.168.1.2",
        .rooms={{.id="RINCON_A",.coordinator="RINCON_A",.name="Kitchen",.ip="192.168.1.2"},
                {.id="RINCON_B",.coordinator="RINCON_B",.name="Study",.ip="192.168.1.3"}}};
    speaker_output_set_completion_cb(finished);
    assert(!speaker_output_select(&rooms,"missing","/first.mp3",0,true));
    assert(speaker_output_select(&rooms,"RINCON_A","/first.mp3",12,false));
    wait_loaded("/first.mp3");wait_state(AUDIO_STATUS_PAUSED);
    pthread_mutex_lock(&fake_lock);assert(plays==0 && remote.position==12);pthread_mutex_unlock(&fake_lock);
    speaker_output_transport(AUDIO_STATUS_PLAYING); wait_state(AUDIO_STATUS_PLAYING);
    pthread_mutex_lock(&fake_lock);remote.position=20;pthread_mutex_unlock(&fake_lock);observe();
    double before=0,after=0;
    speaker_output_progress(&before,NULL);pause_ms(120);speaker_output_progress(&after,NULL);
    assert(after>before+0.08); // fresh reads advance between whole-second reports
    observe();
    double repeated=0;speaker_output_progress(&repeated,NULL);
    assert(repeated>=after); // repeated/late report does not rewind the playhead
    speaker_output_seek(5);observe();
    speaker_output_progress(&repeated,NULL);assert(repeated>=5 && repeated<6); // intentional rewind works
    speaker_output_transport(AUDIO_STATUS_PAUSED); wait_state(AUDIO_STATUS_PAUSED);
    speaker_output_seek(45); speaker_output_set_volume(41);observe();
    double position=0,duration=0;speaker_output_progress(&position,&duration);
    assert(position==45 && duration==100 && speaker_output_volume()==41);
    pthread_mutex_lock(&fake_lock);remote.volume=17;pthread_mutex_unlock(&fake_lock);
    observe();assert(speaker_output_volume()==17); // changes from another controller
    for(int i=0;i<10;i++)speaker_output_change_volume(1);
    for(int n=0;n<1500;n++) {
        pthread_mutex_lock(&fake_lock);bool done=remote.volume==27;pthread_mutex_unlock(&fake_lock);
        if(done)break;
        pause_ms(2);
    }
    observe();assert(speaker_output_volume()==27); // accumulated deltas applied exactly once

    pthread_mutex_lock(&fake_lock);growing=true;pthread_mutex_unlock(&fake_lock);
    speaker_output_play("/growing.mp3",0,false);pause_ms(30);
    speaker_output_set_volume(19);
    for(int n=0;n<1500;n++) {
        pthread_mutex_lock(&fake_lock);bool done=remote.volume==19;pthread_mutex_unlock(&fake_lock);
        if(done)break;
        pause_ms(2);
    }
    pthread_mutex_lock(&fake_lock);assert(remote.volume==19);pthread_mutex_unlock(&fake_lock);
    speaker_output_play("/next.mp3",0,false);wait_loaded("/next.mp3");wait_state(AUDIO_STATUS_PLAYING);
    pthread_mutex_lock(&fake_lock);assert(!strstr(loaded,"growing"));pthread_mutex_unlock(&fake_lock);

    // Pause and volume arrive while SetURI is blocked; neither loses the load.
    pthread_mutex_lock(&fake_lock);block_load=true;load_entered=false;pthread_mutex_unlock(&fake_lock);
    speaker_output_play("/slow.mp3",0,false);
    for(int n=0;n<1500;n++) {
        pthread_mutex_lock(&fake_lock);bool entered=load_entered;pthread_mutex_unlock(&fake_lock);
        if(entered)break;
        pause_ms(2);
    }
    speaker_output_transport(AUDIO_STATUS_PAUSED);speaker_output_set_volume(29);
    pthread_mutex_lock(&fake_lock);block_load=false;pthread_mutex_unlock(&fake_lock);
    wait_loaded("/slow.mp3");wait_state(AUDIO_STATUS_PAUSED);observe();assert(speaker_output_volume()==29);
    speaker_output_transport(AUDIO_STATUS_PLAYING);wait_state(AUDIO_STATUS_PLAYING);observe();
    pthread_mutex_lock(&fake_lock);remote.position=99;pthread_mutex_unlock(&fake_lock);observe();
    pthread_mutex_lock(&fake_lock);snprintf(remote.state,sizeof(remote.state),"STOPPED");remote.position=0;pthread_mutex_unlock(&fake_lock);observe();
    assert(speaker_output_take_completion());assert(!speaker_output_take_completion());
    pthread_mutex_lock(&fake_lock);assert(completions==1);pthread_mutex_unlock(&fake_lock);
    speaker_output_play("/next.mp3",0,false);wait_state(AUDIO_STATUS_PLAYING);observe();
    speaker_output_transport(AUDIO_STATUS_STOPPED);wait_state(AUDIO_STATUS_STOPPED);observe();assert(!speaker_output_take_completion());

    assert(speaker_output_select(&rooms,"RINCON_B","/study.mp3",0,true));
    wait_loaded("/study.mp3");wait_state(AUDIO_STATUS_PLAYING);
    pthread_mutex_lock(&fake_lock);assert(!strcmp(stop_room,"RINCON_A"));online=false;pthread_mutex_unlock(&fake_lock);kick();
    for(int n=0;n<1500&&speaker_output_status()!=AUDIO_STATUS_STOPPED;n++)pause_ms(2);
    assert(speaker_output_status()==AUDIO_STATUS_STOPPED);assert(!speaker_output_take_completion());
    char why[192];speaker_output_error(why,sizeof(why));assert(strstr(why,"disconnected"));
    pthread_mutex_lock(&fake_lock);online=true;pthread_mutex_unlock(&fake_lock);observe();
    pthread_mutex_lock(&fake_lock);snprintf(remote.uri,sizeof(remote.uri),"http://other-controller/track");pthread_mutex_unlock(&fake_lock);observe();
    assert(speaker_output_status()==AUDIO_STATUS_STOPPED);assert(!speaker_output_take_completion());
    observe(); // an external replacement must remain stopped in our facade
    assert(speaker_output_status()==AUDIO_STATUS_STOPPED);
    pthread_mutex_lock(&fake_lock);unsigned old_stops=stops;pthread_mutex_unlock(&fake_lock);
    speaker_output_local();
    for(int n=0;n<1500;n++) {
        pthread_mutex_lock(&fake_lock);bool revoked=!sharing;pthread_mutex_unlock(&fake_lock);if(revoked)break;pause_ms(2);
    }
    assert(!speaker_output_active());
    pthread_mutex_lock(&fake_lock);assert(!sharing && stops==old_stops);pthread_mutex_unlock(&fake_lock); // don't stop someone else's music

#ifdef FULL_APP_TEST
    assert(argc==2);assert(config_init(argv[1]));
    assert(speaker_output_select(&rooms,"RINCON_A","/facade.mp3",0,true));
    wait_loaded("/facade.mp3");wait_state(AUDIO_STATUS_PLAYING);
    audio_pause();wait_state(AUDIO_STATUS_PAUSED);assert(audio_get_status()==AUDIO_STATUS_PAUSED);
    audio_resume();wait_state(AUDIO_STATUS_PLAYING);assert(audio_get_status()==AUDIO_STATUS_PLAYING);
    audio_seek(25);set_volume_percent(32);observe();
    audio_get_progress(&position,&duration);assert(position>=25 && position<26 && duration==100 && get_volume_percent()==32);
    audio_play("/changed.mp3");wait_loaded("/changed.mp3");wait_state(AUDIO_STATUS_PLAYING);
    char path[512];audio_get_current_file(path,sizeof(path));assert(!strcmp(path,"/changed.mp3"));
    const char *tracks[]={"/queue-first.mp3","/queue-second.mp3","/queue-third.mp3"};
    device_state_play_list(tracks,3,0);wait_loaded(tracks[0]);wait_state(AUDIO_STATUS_PLAYING);
    assert(device_state_next(NULL,0));wait_loaded(tracks[1]);wait_state(AUDIO_STATUS_PLAYING);
    assert(device_state_prev(NULL,0));wait_loaded(tracks[0]);wait_state(AUDIO_STATUS_PLAYING);
    device_state_toggle_play_pause();wait_state(AUDIO_STATUS_PAUSED);
    device_state_toggle_play_pause();wait_state(AUDIO_STATUS_PLAYING);
    device_state_change_volume_percent(5);observe();assert(get_volume_percent()==37);
    pthread_mutex_lock(&fake_lock);remote.position=99;pthread_mutex_unlock(&fake_lock);observe();
    pthread_mutex_lock(&fake_lock);snprintf(remote.state,sizeof(remote.state),"STOPPED");remote.position=0;pthread_mutex_unlock(&fake_lock);observe();
    assert(device_state_take_completion());assert(device_state_advance_auto(NULL,0));
    wait_loaded(tracks[1]);wait_state(AUDIO_STATUS_PLAYING);
    device_state_stop();wait_state(AUDIO_STATUS_STOPPED);assert(!device_state_take_completion());
    speaker_output_local();
    puts("Device-wide wireless playback, queue and volume integration tests passed");
#endif
    puts("Wireless output worker tests passed"); return 0;
}
