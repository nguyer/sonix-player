#include "speakers.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/system/audio/audio.h"
#include "src/system/playback/device_state.h"
#include "src/system/remote/sonos.h"
#include "src/system/remote/speaker_output.h"

lv_obj_t *speakers_screen;
enum { AUTO=1, CONNECT_IP, JOIN, LEAVE, VOLUME, LEVELS };
typedef struct {
    int kind, volume;
    char room[SONOS_ID_MAX], target[SONOS_ID_MAX], ip[40];
} job_t;
typedef struct { char id[SONOS_ID_MAX]; int value; bool known; double stamp; bool optimistic; int preview_base; } room_level_t;
typedef struct {
    char id[SONOS_ID_MAX], coordinator[SONOS_ID_MAX];
    lv_obj_t *slider, *value;
} room_widget_t;
#define JOBS_MAX 64
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake=PTHREAD_COND_INITIALIZER;
static bool started, working, result_ready;
static job_t jobs[JOBS_MAX], running;
static int job_count;
static sonos_t snapshot, shown, rendered;
static room_level_t snapshot_levels[SONOS_MAX_ROOMS], levels[SONOS_MAX_ROOMS];
static room_widget_t widgets[SONOS_MAX_ROOMS];
static char rendered_group[SONOS_ID_MAX];
static bool render_valid, render_pending;
static lv_obj_t *page_content, *room_list, *local_play_icon;
static lv_obj_t *sheet_veil, *sheet_card;
static int screen_height, sheet_top, page_top;
static bool sheet_open;
#ifdef HOST_BUILD
static lv_obj_t *ip_editor,*ip_field,*ip_return_screen,*manual_row;
static keyboard_t *ip_keyboard;
static char manual_ip[16];
static bool ip_from_player;
#endif
static void render(void);
static void consume_result(void *unused);
static void sheet_close(void);
static bool submit(const job_t *job);

static int room_index(const sonos_t *s,const char *id) {
    for(int i=0;i<s->count;i++)if(!strcmp(s->rooms[i].id,id))return i;
    return -1;
}
static int group_size(const sonos_t *s,const char *coordinator) {
    int count=0;
    for(int i=0;i<s->count;i++)if(!strcmp(s->rooms[i].coordinator,coordinator))count++;
    return count;
}
static void active_group(char *out,size_t cap) {
    char id[SONOS_ID_MAX];speaker_output_room(id,sizeof(id));
    int i=room_index(&shown,id);
    snprintf(out,cap,"%s",i>=0 ? shown.rooms[i].coordinator : "");
}
static room_level_t *level_for(room_level_t *cache,const char *id) {
    room_level_t *empty=NULL;
    for(int i=0;i<SONOS_MAX_ROOMS;i++) {
        if(!strcmp(cache[i].id,id))return &cache[i];
        if(!cache[i].id[0] && !empty)empty=&cache[i];
    }
    if(empty)snprintf(empty->id,sizeof(empty->id),"%.*s",SONOS_ID_MAX-1,id);
    return empty;
}
static double volume_stamp(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1000000000.0;
}
// A successful SOAP response acknowledges the command before topology has
// necessarily converged. Re-read only during this operation, off the GUI thread.
static bool confirm_group(sonos_t *s,const job_t *job) {
    for(int attempt=0;attempt<30;attempt++) {
        int at=room_index(s,job->room);
        bool confirmed=false;
        if(at>=0 && job->kind==JOIN) {
            int target=room_index(s,job->target);
            confirmed=target>=0 && !strcmp(s->rooms[at].coordinator,s->rooms[target].coordinator);
        } else if(at>=0 && job->kind==LEAVE) {
            confirmed=!strcmp(s->rooms[at].coordinator,job->room) && group_size(s,job->room)==1;
        }
        if(confirmed)return true;
        usleep(100000);
        if(!sonos_refresh(s))return false;
    }
    snprintf(s->error,sizeof(s->error),"Speaker group change could not be confirmed");
    return false;
}
static void read_levels(sonos_t *s,room_level_t *cache) {
    char id[SONOS_ID_MAX];speaker_output_room(id,sizeof(id));
    int selected=room_index(s,id);
    if(selected<0)return;
    for(int i=0;i<s->count;i++)if(!strcmp(s->rooms[i].coordinator,s->rooms[selected].coordinator)) {
        room_level_t *level=level_for(cache,s->rooms[i].id);
        if(level) { level->known=sonos_get_room_volume(s,s->rooms[i].id,&level->value);level->stamp=volume_stamp(); }
    }
}
static void *worker(void *unused) {
    (void)unused;
    sonos_t controller={0};
    room_level_t cache[SONOS_MAX_ROOMS]={0};
    for(;;) {
        pthread_mutex_lock(&lock);
        while(!job_count)pthread_cond_wait(&wake,&lock);
        job_t job=jobs[0];
        memmove(jobs,jobs+1,(--job_count)*sizeof(*jobs));
        running=job;working=true;
        pthread_mutex_unlock(&lock);
        bool ok=false;
        char follow[SONOS_ID_MAX]="", original[SONOS_ID_MAX]="";
        if(job.kind==AUTO) {
            if(controller.seed_ip[0])ok=sonos_refresh(&controller);
#ifdef HOST_BUILD
            if(!ok && job.ip[0])ok=sonos_connect(&controller,job.ip);
#endif
            if(!ok)ok=sonos_discover(&controller);
        } else if(job.kind==CONNECT_IP) {
            ok=sonos_connect(&controller,job.ip);
        } else if(job.kind==JOIN) {
            ok=sonos_join(&controller,job.room,job.target);
            if(ok)ok=confirm_group(&controller,&job);
        } else if(job.kind==LEAVE) {
            // If the selected room leaves, keep output on a remaining member.
            speaker_output_room(original,sizeof(original));
            if(sonos_refresh(&controller)) {
                int at=room_index(&controller,job.room);
                if(at>=0 && !strcmp(original,job.room))
                    for(int i=0;i<controller.count;i++)
                        if(strcmp(controller.rooms[i].id,job.room) &&
                           !strcmp(controller.rooms[i].coordinator,controller.rooms[at].coordinator)) {
                            snprintf(follow,sizeof(follow),"%s",controller.rooms[i].id);break;
                        }
                ok=sonos_leave(&controller,job.room);
                if(ok)ok=confirm_group(&controller,&job);
                if(ok && follow[0]) {
                    int at=room_index(&controller,follow);
                    char current[SONOS_ID_MAX];speaker_output_room(current,sizeof(current));
                    if(at>=0 && !strcmp(current,original))
                        speaker_output_follow(&controller,controller.rooms[at].coordinator);
                }
            }
        } else if(job.kind==VOLUME) {
            ok=sonos_volume(&controller,job.room,job.volume);
            room_level_t *level=level_for(cache,job.room);
            if(level) {
                if(ok) { level->known=true;level->value=job.volume;level->stamp=volume_stamp(); }
                else {
                    char failure[sizeof(controller.error)];snprintf(failure,sizeof(failure),"%s",controller.error);
                    level->known=sonos_get_room_volume(&controller,job.room,&level->value);level->stamp=volume_stamp();
                    snprintf(controller.error,sizeof(controller.error),"%s",failure);
                }
            }
        } else if(job.kind==LEVELS) {
            ok=sonos_refresh(&controller);
        }
        // Discovery may legitimately find nothing. Explicit actions report
        // failures as popups, without adding status text to the picker.
        if(!ok && job.kind!=AUTO && job.kind!=LEVELS)
            gui_notify_popup(controller.error[0] ? controller.error : "Speaker command failed");
        if(ok && job.kind!=VOLUME) {
            memset(cache,0,sizeof(cache));read_levels(&controller,cache);
        }
        pthread_mutex_lock(&lock);
        snapshot=controller;memcpy(snapshot_levels,cache,sizeof(cache));
        result_ready=true;working=false;
        pthread_mutex_unlock(&lock);
        gui_post_coalesced(consume_result,NULL);
    }
    return NULL;
}
// Slider requests coalesce by immutable room ID. A last drag value is never
// dropped merely because an earlier network request is still running.
static bool submit(const job_t *job) {
    pthread_mutex_lock(&lock);
    if(!started) {
        pthread_t thread;
        if(pthread_create(&thread,NULL,worker,NULL)) {
            pthread_mutex_unlock(&lock);gui_notify_popup("Cannot start speaker controller");return false;
        }
        pthread_detach(thread);started=true;
    }
    for(int i=0;i<job_count;i++) {
        if(jobs[i].kind==job->kind &&
           (job->kind==AUTO || job->kind==LEVELS ||
            (job->kind==VOLUME && !strcmp(jobs[i].room,job->room)))) {
            jobs[i]=*job;pthread_mutex_unlock(&lock);return true;
        }
    }
    if(job_count==JOBS_MAX) {
        pthread_mutex_unlock(&lock);gui_notify_popup("Please wait for speaker changes to finish");return false;
    }
    jobs[job_count++]=*job;pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);return true;
}
static void request_refresh(void) {
    job_t job={.kind=AUTO};
#ifdef HOST_BUILD
    snprintf(job.ip,sizeof(job.ip),"%s",manual_ip);
#endif
    submit(&job);
}
static bool volume_pending(const char *id) {
    pthread_mutex_lock(&lock);
    bool pending=working && running.kind==VOLUME && !strcmp(running.room,id);
    for(int i=0;i<job_count;i++)if(jobs[i].kind==VOLUME && !strcmp(jobs[i].room,id))pending=true;
    pthread_mutex_unlock(&lock);return pending;
}
static bool pressed_slider(void) {
    for(int i=0;i<SONOS_MAX_ROOMS;i++)
        if(widgets[i].slider && lv_obj_has_state(widgets[i].slider,LV_STATE_PRESSED))return true;
    return false;
}
static void update_levels(void) {
    for(int i=0;i<SONOS_MAX_ROOMS;i++) {
        room_widget_t *w=&widgets[i];
        if(!w->slider || lv_obj_has_state(w->slider,LV_STATE_PRESSED) || volume_pending(w->id))continue;
        room_level_t *level=level_for(levels,w->id);
        bool known=level && level->known;
        if(known) {
            lv_obj_remove_state(w->slider,LV_STATE_DISABLED);
            lv_slider_set_value(w->slider,level->value,LV_ANIM_OFF);
            lv_label_set_text_fmt(w->value,"%d%%",level->value);
        } else {
            lv_obj_add_state(w->slider,LV_STATE_DISABLED);
            lv_label_set_text(w->value,"—");
        }
    }
}
static bool topology_equal(const sonos_t *a,const sonos_t *b) {
    if(a->count!=b->count)return false;
    for(int i=0;i<a->count;i++)
        if(strcmp(a->rooms[i].id,b->rooms[i].id) ||
           strcmp(a->rooms[i].coordinator,b->rooms[i].coordinator) ||
           strcmp(a->rooms[i].name,b->rooms[i].name))return false;
    return true;
}
static void local_output_cb(lv_event_t *event) {
    (void)event;
    if(!speaker_output_active())return;
    device_state_t state;device_state_get(&state);
    speaker_output_local();player_refresh_wireless_output();
    if(state.current_file[0]) {
        if(state.status==AUDIO_STATUS_PLAYING)audio_play_at(state.current_file,state.progress_current_secs);
        else audio_play_paused(state.current_file,state.progress_current_secs);
    }
    render();
}
static void select_cb(lv_event_t *event) {
    room_widget_t *w=lv_event_get_user_data(event);
    if(!w)return;
    char group[SONOS_ID_MAX];active_group(group,sizeof(group));
    if(!strcmp(group,w->coordinator))return;
    device_state_t state;device_state_get(&state);
    if(state.live) { gui_notify_popup("Wireless playback supports music files, not live radio");return; }
    bool local=!speaker_output_active();
    if(local)audio_stop();
    if(!speaker_output_select(&shown,w->coordinator,state.current_file,state.progress_current_secs,
                              state.status==AUDIO_STATUS_PLAYING)) {
        if(local && state.current_file[0]) {
            if(state.status==AUDIO_STATUS_PLAYING)audio_play_at(state.current_file,state.progress_current_secs);
            else audio_play_paused(state.current_file,state.progress_current_secs);
        }
        gui_notify_popup("Cannot select that speaker");return;
    }
    player_refresh_wireless_output();render();
    job_t job={.kind=LEVELS};submit(&job);
}
static void group_cb(lv_event_t *event) {
    room_widget_t *w=lv_event_get_user_data(event);
    if(!w)return;
    int at=room_index(&shown,w->id);if(at<0)return;
    job_t job={0};snprintf(job.room,sizeof(job.room),"%s",w->id);
    if(group_size(&shown,w->coordinator)>1)job.kind=LEAVE;
    else {
        active_group(job.target,sizeof(job.target));
        if(!job.target[0] || !strcmp(job.target,w->coordinator))return;
        job.kind=JOIN;
    }
    submit(&job);
}
static void slider_cb(lv_event_t *event) {
    room_widget_t *w=lv_event_get_user_data(event);
    if(lv_event_get_code(event)==LV_EVENT_VALUE_CHANGED) {
        int value=lv_slider_get_value(w->slider);
        lv_label_set_text_fmt(w->value,"%d%%",value);
        room_level_t *level=level_for(levels,w->id);
        if(level) { level->known=true;level->value=value;level->stamp=volume_stamp();level->optimistic=false; }
        job_t job={.kind=VOLUME,.volume=value};
        snprintf(job.room,sizeof(job.room),"%s",w->id);submit(&job);
    } else if(lv_event_get_code(event)==LV_EVENT_RELEASED && render_pending)render();
}
static void style_column(lv_obj_t *object) {
    lv_obj_set_width(object,lv_pct(100));lv_obj_set_height(object,LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(object,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(object,0,0);
    lv_obj_set_style_pad_all(object,0,0);lv_obj_set_style_pad_gap(object,8,0);
    lv_obj_set_flex_flow(object,LV_FLEX_FLOW_COLUMN);lv_obj_set_scrollable(object,false);
}
static lv_obj_t *play_icon(lv_obj_t *parent,bool active) {
    lv_obj_t *icon=lv_image_create(parent);lv_image_set_src(icon,&icon_play_small);
    lv_obj_add_style(icon,&theme_style_icon,0);lv_obj_set_hidden(icon,!active);
    return icon;
}
static lv_obj_t *icon_button(lv_obj_t *parent,const lv_image_dsc_t *icon,lv_event_cb_t callback,void *user) {
    lv_obj_t *button=lv_btn_create(parent);
    lv_obj_set_size(button,44,44);lv_obj_set_style_bg_opa(button,LV_OPA_TRANSP,0);
    lv_obj_set_style_shadow_width(button,0,0);lv_obj_set_event_bubble(button,false);
    lv_obj_add_event_cb(button,callback,LV_EVENT_CLICKED,user);
    lv_obj_t *image=lv_image_create(button);lv_image_set_src(image,icon);
    lv_obj_add_style(image,&theme_style_icon,0);lv_obj_center(image);
    return button;
}
static lv_obj_t *header(lv_obj_t *parent,const char *title,lv_event_cb_t cb,void *user,bool active) {
    lv_obj_t *row=lv_btn_create(parent);
    lv_obj_set_size(row,lv_pct(100),56);lv_obj_set_style_bg_opa(row,LV_OPA_TRANSP,0);
    lv_obj_set_style_shadow_width(row,0,0);lv_obj_set_style_pad_all(row,4,0);
    lv_obj_set_style_pad_gap(row,8,0);lv_obj_set_flex_flow(row,LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
    lv_obj_set_event_bubble(row,false);
    if(cb)lv_obj_add_event_cb(row,cb,LV_EVENT_CLICKED,user);
    play_icon(row,active);
    lv_obj_t *name=lv_label_create(row);lv_label_set_text(name,title);
    lv_obj_add_style(name,&theme_style_text,0);lv_obj_set_style_text_font(name,&font_ui_24,0);
    lv_obj_set_flex_grow(name,1);lv_label_set_long_mode(name,LV_LABEL_LONG_DOT);
    return row;
}
static void room_row(lv_obj_t *parent,int index,bool grouped,const char *active) {
    const sonos_room_t *r=&shown.rooms[index];room_widget_t *w=&widgets[index];
    snprintf(w->id,sizeof(w->id),"%s",r->id);
    snprintf(w->coordinator,sizeof(w->coordinator),"%s",r->coordinator);
    bool current=!strcmp(active,r->coordinator);
    lv_obj_t *card=lv_obj_create(parent);lv_obj_set_width(card,lv_pct(100));
    lv_obj_set_height(card,current ? 122 : 72);lv_obj_set_scrollable(card,false);
    lv_obj_set_style_border_width(card,0,0);lv_obj_set_style_pad_all(card,8,0);
    if(!grouped)lv_obj_add_style(card,&theme_style_card,0);
    else lv_obj_set_style_bg_opa(card,LV_OPA_TRANSP,0);
    lv_obj_t *row=header(card,r->name,select_cb,w,current && !grouped);
    if(current) {
        w->value=lv_label_create(row);lv_obj_add_style(w->value,&theme_style_text_dim,0);
        lv_obj_set_style_text_font(w->value,&font_ui_20,0);lv_label_set_text(w->value,"—");
        lv_obj_set_width(w->value,52);
    }
    lv_obj_t *button=icon_button(row,grouped ? &icon_close : &icon_plus,group_cb,w);
    if(!grouped && (!active[0] || current))lv_obj_add_state(button,LV_STATE_DISABLED);
    if(current) {
        w->slider=lv_slider_create(card);lv_slider_set_range(w->slider,0,100);
        lv_obj_set_size(w->slider,lv_pct(90),8);
        lv_obj_align(w->slider,LV_ALIGN_BOTTOM_MID,0,-10);
        lv_obj_add_style(w->slider,&theme_style_slider_track,LV_PART_MAIN);
        lv_obj_add_style(w->slider,&theme_style_accent_bg,LV_PART_INDICATOR);
        lv_obj_set_style_radius(w->slider,LV_RADIUS_CIRCLE,LV_PART_MAIN);
        lv_obj_set_style_radius(w->slider,LV_RADIUS_CIRCLE,LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(w->slider,lv_color_white(),LV_PART_KNOB);
        lv_obj_set_style_radius(w->slider,LV_RADIUS_CIRCLE,LV_PART_KNOB);
        lv_obj_set_style_pad_all(w->slider,7,LV_PART_KNOB);
        lv_obj_set_ext_click_area(w->slider,14);
        theme_apply_slider_knob(w->slider);
        lv_obj_add_event_cb(w->slider,slider_cb,LV_EVENT_VALUE_CHANGED,w);
        lv_obj_add_event_cb(w->slider,slider_cb,LV_EVENT_RELEASED,w);
    }
}
static void render(void) {
    char active[SONOS_ID_MAX];active_group(active,sizeof(active));
    lv_obj_set_hidden(local_play_icon,speaker_output_active());
    if(render_valid && topology_equal(&shown,&rendered) && !strcmp(active,rendered_group)) {
        update_levels();return;
    }
    if(pressed_slider()) { render_pending=true;return; }
    render_pending=false;
    int scroll=lv_obj_get_scroll_y(page_content);
    memset(widgets,0,sizeof(widgets));lv_obj_clean(room_list);
    bool used[SONOS_MAX_ROOMS]={0};
    for(int i=0;i<shown.count;i++) {
        if(used[i])continue;
        const char *group=shown.rooms[i].coordinator;
        int count=group_size(&shown,group);
        if(count==1) {
            used[i]=true;room_row(room_list,i,false,active);continue;
        }
        lv_obj_t *card=lv_obj_create(room_list);style_column(card);
        lv_obj_add_style(card,&theme_style_card,0);lv_obj_set_style_bg_opa(card,LV_OPA_COVER,0);lv_obj_set_style_pad_all(card,8,0);
        int coordinator=room_index(&shown,group);
        if(coordinator<0)continue;
        char title[180];snprintf(title,sizeof(title),"%s + %d",shown.rooms[coordinator].name,count-1);
        room_widget_t *w=&widgets[coordinator];
        snprintf(w->id,sizeof(w->id),"%s",group);snprintf(w->coordinator,sizeof(w->coordinator),"%s",group);
        header(card,title,select_cb,w,!strcmp(active,group));
        for(int j=0;j<shown.count;j++)if(!strcmp(shown.rooms[j].coordinator,group)) {
            used[j]=true;room_row(card,j,true,active);
        }
    }
    rendered=shown;snprintf(rendered_group,sizeof(rendered_group),"%s",active);render_valid=true;
    update_levels();lv_obj_update_layout(page_content);lv_obj_scroll_to_y(page_content,scroll,LV_ANIM_OFF);
}
static void consume_result(void *unused) {
    (void)unused;
    pthread_mutex_lock(&lock);
    bool changed=result_ready;
    if(changed) {
        shown=snapshot;
        for(int i=0;i<SONOS_MAX_ROOMS;i++)
            if(levels[i].id[0] && room_index(&shown,levels[i].id)<0)memset(&levels[i],0,sizeof(levels[i]));
        for(int i=0;i<SONOS_MAX_ROOMS;i++)if(snapshot_levels[i].known) {
            room_level_t *level=level_for(levels,snapshot_levels[i].id);
            if(level && snapshot_levels[i].stamp>=level->stamp)*level=snapshot_levels[i];
        }
        result_ready=false;
    }
    pthread_mutex_unlock(&lock);
    if(changed)render();
}
static void output_changed_ui(void *unused) {
    (void)unused;player_refresh_wireless_output();
    static unsigned volume_revision;
    unsigned revision=speaker_output_group_volume_revision();
    bool group_volume_changed=revision!=volume_revision;volume_revision=revision;
    char before[SONOS_ID_MAX];snprintf(before,sizeof(before),"%s",rendered_group);
    char error[192];speaker_output_error(error,sizeof(error));
    bool pending=speaker_output_group_volume_pending(), released=false;
    if(!pending)for(int i=0;i<SONOS_MAX_ROOMS;i++)if(levels[i].optimistic) {
        if(error[0])levels[i].value=levels[i].preview_base;
        levels[i].optimistic=false;levels[i].stamp=volume_stamp();released=true;
    }
    char room[SONOS_ID_MAX];int percent;double stamp;
    if(speaker_output_volume_feedback(room,sizeof(room),&percent,&stamp) && !volume_pending(room)) {
        room_level_t *level=level_for(levels,room);
        if(level && stamp>=level->stamp) {
            level->known=true;level->value=percent;level->stamp=stamp;
        }
    }
    render();
    if(group_volume_changed || released || strcmp(before,rendered_group)) { job_t job={.kind=LEVELS};submit(&job); }
    static char previous_error[192];
    if(error[0] && strcmp(error,previous_error))gui_notify_popup(error);
    snprintf(previous_error,sizeof(previous_error),"%s",error);
}
typedef struct {
    char room[SONOS_ID_MAX];
    int value;
    bool absolute;
    double stamp;
} volume_preview_t;
static void volume_preview_ui(void *arg) {
    volume_preview_t preview=*(volume_preview_t *)arg;free(arg);
    char active[SONOS_ID_MAX];active_group(active,sizeof(active));
    int at=room_index(&shown,preview.room);
    if(at<0 || !active[0] || strcmp(shown.rooms[at].coordinator,active))return;
    int delta=preview.value;
    if(preview.absolute) {
        room_level_t *anchor=level_for(levels,preview.room);
        if(!anchor || !anchor->known)return;
        if(!anchor->optimistic && anchor->stamp>=preview.stamp)return;
        delta-=anchor->value;
    }
    for(int i=0;i<shown.count;i++)if(!strcmp(shown.rooms[i].coordinator,active)) {
        room_level_t *level=level_for(levels,shown.rooms[i].id);
        if(!level || !level->known)continue;
        // A fresh reply might have reached the GUI before this queued preview.
        if(!level->optimistic && level->stamp>=preview.stamp)continue;
        if(!level->optimistic)level->preview_base=level->value;
        int value=level->value+delta;
        level->value=value<0 ? 0 : value>100 ? 100 : value;
        level->optimistic=true;level->stamp=INFINITY;
    }
    // All widgets change in this one GUI pass. Partial speaker replies cannot
    // overwrite predictions while the group command is still being sent.
    update_levels();
    if(!speaker_output_group_volume_pending())output_changed_ui(NULL);
}
static void volume_preview(const char *room,int value,bool absolute,double stamp) {
    volume_preview_t *preview=malloc(sizeof(*preview));
    if(!preview)return;
    snprintf(preview->room,sizeof(preview->room),"%s",room);
    preview->value=value;preview->absolute=absolute;preview->stamp=stamp;
    if(!gui_post(volume_preview_ui,preview))free(preview);
}
static void output_changed(void) { gui_post_coalesced(output_changed_ui,NULL); }
#ifdef HOST_BUILD
static void ip_accept_cb(lv_event_t *event) {
    (void)event;job_t job={.kind=CONNECT_IP};
    snprintf(job.ip,sizeof(job.ip),"%s",lv_textarea_get_text(ip_field));
    snprintf(manual_ip,sizeof(manual_ip),"%.15s",job.ip);
    keyboard_set_visible(ip_keyboard,false);
    switch_screen_return_to(ip_return_screen ? ip_return_screen : speakers_screen);submit(&job);
}
static void reopen_sheet(void *unused) {
    (void)unused;if(ip_from_player)player_sheet_open(false);speakers_output_open();
}
static void ip_editor_left_cb(lv_event_t *event) {
    (void)event;keyboard_set_visible(ip_keyboard,false);gui_post_coalesced(reopen_sheet,NULL);
}
static void ip_edit_cb(lv_event_t *event) {
    (void)event;lv_textarea_set_text(ip_field,manual_ip);
    keyboard_reset(ip_keyboard);keyboard_set_visible(ip_keyboard,true);
    ip_return_screen=lv_screen_active();ip_from_player=player_sheet_is_open();
    sheet_close();if(sheet_veil)lv_obj_set_hidden(sheet_veil,true);
    if(ip_from_player)player_sheet_close(false);
    switch_screen(ip_editor);
}
#endif
static void screen_opened(lv_event_t *event) { (void)event;render();request_refresh(); }
static void refresh_theme(void) {
    for(int i=0;i<SONOS_MAX_ROOMS;i++)if(widgets[i].slider)theme_apply_slider_knob(widgets[i].slider);
}
void speakers_page_init(gui_config_t *cfg) {
    lv_obj_add_style(speakers_screen,&theme_style_screen,0);settingsrow_title(speakers_screen,cfg,"Output");
    page_top=settingsrow_content_top(cfg);screen_height=cfg->screen_height;
    page_content=lv_obj_create(speakers_screen);
    lv_obj_set_size(page_content,lv_pct(100),screen_height-page_top);lv_obj_set_pos(page_content,0,page_top);
    lv_obj_set_style_bg_opa(page_content,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(page_content,0,0);
    lv_obj_set_style_pad_hor(page_content,cfg->padding,0);lv_obj_set_style_radius(page_content,0,0);
    lv_obj_set_flex_flow(page_content,LV_FLEX_FLOW_COLUMN);lv_obj_set_style_pad_gap(page_content,12,0);
    lv_obj_t *local=lv_obj_create(page_content);lv_obj_set_size(local,lv_pct(100),72);
    lv_obj_set_scrollable(local,false);lv_obj_add_style(local,&theme_style_card,0);
    lv_obj_set_style_pad_all(local,8,0);lv_obj_set_style_border_width(local,0,0);
    lv_obj_t *row=header(local,"This device",local_output_cb,NULL,true);
    local_play_icon=lv_obj_get_child(row,0);
    room_list=lv_obj_create(page_content);style_column(room_list);
#ifdef HOST_BUILD
    manual_row=settingsrow_action(page_content,"Connect by speaker IP",ip_edit_cb,NULL);
    ip_editor=lv_obj_create(NULL);lv_obj_add_style(ip_editor,&theme_style_screen,0);
    settingsrow_title(ip_editor,cfg,"Speaker IPv4 address");
    ip_field=lv_textarea_create(ip_editor);lv_textarea_set_one_line(ip_field,true);
    lv_textarea_set_max_length(ip_field,15);lv_textarea_set_accepted_chars(ip_field,"0123456789.");
    lv_textarea_set_placeholder_text(ip_field,"192.168.1.50");
    lv_obj_set_size(ip_field,cfg->screen_width-2*cfg->padding,62);
    lv_obj_set_pos(ip_field,cfg->padding,page_top);lv_obj_add_style(ip_field,&theme_style_card,0);
    lv_obj_set_style_text_font(ip_field,&font_ui_24,0);keyboard_style_caret(ip_field);
    ip_keyboard=keyboard_create(ip_editor,cfg->screen_width,316,ip_field,NULL,"ok",ip_accept_cb,NULL);
    keyboard_set_visible(ip_keyboard,false);
    lv_obj_add_event_cb(ip_editor,ip_editor_left_cb,LV_EVENT_SCREEN_UNLOADED,NULL);
    switcher_attach_back_gesture(ip_editor);
#endif
    speaker_output_set_changed_cb(output_changed);speaker_output_set_volume_preview_cb(volume_preview);theme_register_refresh(refresh_theme);
    lv_obj_add_event_cb(speakers_screen,screen_opened,LV_EVENT_SCREEN_LOADED,NULL);
    render();
}

static void sheet_y(void *object, int32_t y) { lv_obj_set_y(object,y); }
static void sheet_hidden(lv_anim_t *animation) {
    (void)animation;
    if (!sheet_open) {
        lv_obj_set_parent(page_content,speakers_screen);
        lv_obj_set_size(page_content,lv_pct(100),screen_height-page_top);
        lv_obj_set_pos(page_content,0,page_top);
        lv_obj_set_hidden(sheet_veil,true);
    }
}
static void sheet_close(void) {
    if (!sheet_veil || !sheet_open) return;
    sheet_open=false;
    lv_anim_delete(sheet_card,sheet_y);
    lv_anim_t animation; lv_anim_init(&animation);
    lv_anim_set_var(&animation,sheet_card); lv_anim_set_exec_cb(&animation,sheet_y);
    lv_anim_set_values(&animation,lv_obj_get_y(sheet_card),screen_height);
    lv_anim_set_duration(&animation,220); lv_anim_set_path_cb(&animation,lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&animation,sheet_hidden); lv_anim_start(&animation);
}
static void close_cb(lv_event_t *event) { (void)event; sheet_close(); }
void speakers_output_open(void) {
    if (!page_content) return;
    if (!sheet_veil) {
        sheet_veil=lv_obj_create(lv_layer_top());
        lv_obj_set_size(sheet_veil,lv_pct(100),lv_pct(100)); lv_obj_set_pos(sheet_veil,0,0);
        lv_obj_set_style_bg_color(sheet_veil,lv_color_black(),0);
        lv_obj_set_style_bg_opa(sheet_veil,LV_OPA_50,0);
        lv_obj_set_style_border_width(sheet_veil,0,0); lv_obj_set_style_pad_all(sheet_veil,0,0);
        lv_obj_set_scrollable(sheet_veil,false);
        lv_obj_add_event_cb(sheet_veil,close_cb,LV_EVENT_CLICKED,NULL);
        sheet_card=lv_obj_create(sheet_veil);
        sheet_top=screen_height/8;
        lv_obj_set_size(sheet_card,lv_pct(100),screen_height-sheet_top);
        lv_obj_add_style(sheet_card,&theme_style_panel,0);
        lv_obj_set_style_pad_all(sheet_card,0,0); lv_obj_set_style_border_width(sheet_card,0,0);
        lv_obj_set_style_radius(sheet_card,24,0); lv_obj_set_scrollable(sheet_card,false);
        lv_obj_set_event_bubble(sheet_card,false);
        lv_obj_t *title=lv_label_create(sheet_card);
        lv_label_set_text(title,"Output"); lv_obj_add_style(title,&theme_style_text,0);
        lv_obj_set_style_text_font(title,&font_ui_28,0); lv_obj_set_pos(title,20,20);
        lv_obj_t *close=lv_btn_create(sheet_card);
        lv_obj_set_size(close,48,48); lv_obj_align(close,LV_ALIGN_TOP_RIGHT,-8,8);
        lv_obj_set_style_bg_opa(close,LV_OPA_TRANSP,0); lv_obj_set_style_shadow_width(close,0,0);
        lv_obj_t *label=lv_image_create(close); lv_image_set_src(label,&icon_close);
        lv_obj_add_style(label,&theme_style_icon,0); lv_obj_center(label);
        lv_obj_add_event_cb(close,close_cb,LV_EVENT_CLICKED,NULL);
    }
    sheet_open=true; lv_anim_delete(sheet_card,sheet_y);
    lv_obj_set_parent(page_content,sheet_card);
    lv_obj_set_size(page_content,lv_pct(100),screen_height-sheet_top-76);
    lv_obj_set_pos(page_content,0,76);
    lv_obj_set_hidden(sheet_veil,false); lv_obj_move_foreground(sheet_veil);
    render();
    request_refresh();
    lv_anim_t animation; lv_anim_init(&animation); lv_anim_set_var(&animation,sheet_card);
    lv_anim_set_exec_cb(&animation,sheet_y); lv_anim_set_values(&animation,screen_height,sheet_top);
    lv_anim_set_duration(&animation,240); lv_anim_set_path_cb(&animation,lv_anim_path_ease_out);
    lv_anim_start(&animation);
}
