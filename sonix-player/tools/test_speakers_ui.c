#include "src/gui/wireless/speakers.c"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include "src/system/net/wifi.h"
#include "src/system/audio/alsa-controls.h"
#include "src/system/core/config.h"
#include "src/gui/shell/theme.h"
#include "src/gui/nowplaying/player.h"
static void save_snapshot(lv_obj_t *object,const char *path) {
    lv_draw_buf_t *buffer=lv_snapshot_take(object,LV_COLOR_FORMAT_RGB888);assert(buffer);
    FILE *file=fopen(path,"wb");assert(file);
    fprintf(file,"P6\n%u %u\n255\n",buffer->header.w,buffer->header.h);
    for(unsigned y=0;y<buffer->header.h;y++)for(unsigned x=0;x<buffer->header.w;x++) {
        unsigned char *b=buffer->data+y*buffer->header.stride+x*3;
        fputc(b[2],file);fputc(b[1],file);fputc(b[0],file);
    }
    fclose(file);lv_draw_buf_destroy(buffer);
}
static lv_obj_t *image_parent(lv_obj_t *object,const lv_image_dsc_t *source) {
    if(lv_obj_check_type(object,&lv_image_class) && lv_image_get_src(object)==source)return lv_obj_get_parent(object);
    for(unsigned i=0;i<lv_obj_get_child_count(object);i++) {
        lv_obj_t *found=image_parent(lv_obj_get_child(object,i),source);if(found)return found;
    }
    return NULL;
}

static pthread_mutex_t mock_lock=PTHREAD_MUTEX_INITIALIZER;
static sonos_t household={.count=4,.seed_ip="192.168.1.2",.rooms={
    {.id="RINCON_A",.coordinator="RINCON_A",.name="Living room",.ip="192.168.1.2"},
    {.id="RINCON_B",.coordinator="RINCON_A",.name="Kitchen",.ip="192.168.1.3"},
    {.id="RINCON_C",.coordinator="RINCON_C",.name="Office",.ip="192.168.1.4"},
    {.id="RINCON_D",.coordinator="RINCON_D",.name="Bedroom",.ip="192.168.1.5"}}};
static sonos_t pending_household;
static int pending_reads;
static bool pending_topology, wifi_online=true, block_group_volume;
static int mock_volume[4]={20,35,50,25}, discoveries, refreshes, volume_writes;
static char last_volume_room[SONOS_ID_MAX], last_join[SONOS_ID_MAX], last_target[SONOS_ID_MAX], last_leave[SONOS_ID_MAX];
bool __wrap_wifi_available(void) { return true; }
void __wrap_wifi_get_status(wifi_status_t *out) {
    *out=(wifi_status_t){.state=wifi_online ? WIFI_STATE_CONNECTED : WIFI_STATE_ON};
    if(wifi_online)snprintf(out->ip,sizeof(out->ip),"192.168.1.10");
}
bool __wrap_sonos_discover(sonos_t *s) { pthread_mutex_lock(&mock_lock);*s=household;discoveries++;pthread_mutex_unlock(&mock_lock);return true; }
bool __wrap_sonos_refresh(sonos_t *s) {
    pthread_mutex_lock(&mock_lock);
    if(pending_topology) {
        if(pending_reads>0)pending_reads--;
        else { household=pending_household;pending_topology=false; }
    }
    *s=household;refreshes++;pthread_mutex_unlock(&mock_lock);return true;
}
bool __wrap_sonos_connect(sonos_t *s,const char *ip) { (void)ip;return __wrap_sonos_refresh(s); }
bool __wrap_sonos_get_playback(sonos_t *s,const char *id,sonos_playback_t *out) {
    (void)s;pthread_mutex_lock(&mock_lock);int at=room_index(&household,id);
    *out=(sonos_playback_t){.state="STOPPED",.volume=at>=0 ? mock_volume[at] : 0};
    pthread_mutex_unlock(&mock_lock);return true;
}
bool __wrap_sonos_get_room_volume(sonos_t *s,const char *id,int *value) {
    (void)s;pthread_mutex_lock(&mock_lock);int at=room_index(&household,id);
    if(at>=0)*value=mock_volume[at];
    pthread_mutex_unlock(&mock_lock);return at>=0;
}
bool __wrap_sonos_volume(sonos_t *s,const char *id,int value) {
    usleep(10000); // leave room for slider commands to overlap
    pthread_mutex_lock(&mock_lock);int at=room_index(&household,id);
    if(at>=0)mock_volume[at]=value;
    snprintf(last_volume_room,sizeof(last_volume_room),"%s",id);volume_writes++;
    *s=household;pthread_mutex_unlock(&mock_lock);return at>=0;
}
bool __wrap_sonos_group_volume(sonos_t *s,const char *id,int value,bool absolute) {
    for(;;) {
        pthread_mutex_lock(&mock_lock);bool blocked=block_group_volume;pthread_mutex_unlock(&mock_lock);
        if(!blocked)break;
        usleep(1000);
    }
    pthread_mutex_lock(&mock_lock);int at=room_index(&household,id);assert(at>=0);
    int delta=absolute ? value-mock_volume[at] : value;
    for(int i=0;i<household.count;i++)if(!strcmp(household.rooms[i].coordinator,household.rooms[at].coordinator)) {
        int next=mock_volume[i]+delta;
        mock_volume[i]=next<0 ? 0 : next>100 ? 100 : next;
    }
    *s=household;pthread_mutex_unlock(&mock_lock);return true;
}
bool __wrap_sonos_join(sonos_t *s,const char *id,const char *target) {
    pthread_mutex_lock(&mock_lock);
    int at=room_index(&household,id),to=room_index(&household,target);
    assert(at>=0 && to>=0);memmove(household.rooms[at].coordinator,household.rooms[to].coordinator,SONOS_ID_MAX);
    snprintf(last_join,sizeof(last_join),"%s",id);snprintf(last_target,sizeof(last_target),"%s",target);
    pending_household=household;pending_topology=true;pending_reads=2;
    *s=household;
    snprintf(household.rooms[at].coordinator,SONOS_ID_MAX,"%s",id);
    *s=household;pthread_mutex_unlock(&mock_lock);return true;
}
bool __wrap_sonos_leave(sonos_t *s,const char *id) {
    pthread_mutex_lock(&mock_lock);int at=room_index(&household,id);assert(at>=0);
    sonos_t before=household;
    char old[SONOS_ID_MAX],replacement[SONOS_ID_MAX]="";
    snprintf(old,sizeof(old),"%s",household.rooms[at].coordinator);
    if(!strcmp(old,id)) {
        for(int i=0;i<household.count;i++)if(i!=at && !strcmp(household.rooms[i].coordinator,old)) {
            snprintf(replacement,sizeof(replacement),"%s",household.rooms[i].id);break;
        }
        if(replacement[0])for(int i=0;i<household.count;i++)
            if(i!=at && !strcmp(household.rooms[i].coordinator,old))
                snprintf(household.rooms[i].coordinator,SONOS_ID_MAX,"%s",replacement);
    }
    snprintf(household.rooms[at].coordinator,SONOS_ID_MAX,"%s",id);
    snprintf(last_leave,sizeof(last_leave),"%s",id);
    pending_household=household;pending_topology=true;pending_reads=2;household=before;*s=household;
    pthread_mutex_unlock(&mock_lock);return true;
}
static void settle(void) {
    for(int i=0;i<5000;i++) {
        gui_post_drain();
        pthread_mutex_lock(&lock);bool idle=!working && !job_count && !result_ready;pthread_mutex_unlock(&lock);
        if(idle && !speaker_output_group_volume_pending()) { usleep(20000);gui_post_drain();
            pthread_mutex_lock(&lock);idle=!working && !job_count && !result_ready;pthread_mutex_unlock(&lock);
            if(idle && !speaker_output_group_volume_pending())return;
        }
        usleep(1000);
    }
    assert(!"output worker did not settle");
}
static lv_obj_t *find_text(lv_obj_t *parent,const char *text) {
    if(lv_obj_check_type(parent,&lv_label_class) && !strcmp(lv_label_get_text(parent),text))return parent;
    for(unsigned i=0;i<lv_obj_get_child_count(parent);i++) {
        lv_obj_t *found=find_text(lv_obj_get_child(parent,i),text);if(found)return found;
    }
    return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2);assert(config_init(argv[1]));
    lv_init();assert(lv_display_create(480,720));assert(fonts_init());theme_init();
    gui_config_t cfg={.screen_width=480,.screen_height=720,.padding=16,.top_bar_height=32,.sd_root_path="/tmp"};
    speakers_screen=lv_obj_create(NULL);speakers_page_init(&cfg);
    player_screen=lv_obj_create(NULL);player_init(&cfg);
    lv_obj_set_hidden(player_screen,false);lv_obj_set_pos(player_screen,0,0);
    lv_screen_load(player_screen);lv_obj_update_layout(player_screen);
    lv_obj_t *output=image_parent(player_screen,&icon_airplay_quick);assert(output);
    lv_obj_t *repeat=image_parent(player_screen,&icon_repeat_off);
    if(!repeat)repeat=image_parent(player_screen,&icon_repeat_all);
    assert(repeat);
    lv_obj_t *more=image_parent(player_screen,&icon_ellipsis_vertical);assert(more);
    lv_obj_t *next=image_parent(player_screen,&icon_skip_forward);assert(next);
    lv_area_t a,b,c;lv_obj_get_coords(output,&a);lv_obj_get_coords(more,&b);lv_obj_get_coords(next,&c);
    printf("Wireless button %d,%d-%d,%d; menu %d,%d-%d,%d; next ends at %d\\n",
        a.x1,a.y1,a.x2,a.y2,b.x1,b.y1,b.x2,b.y2,c.x2);
    assert(a.x2 < b.x1 && a.y1==b.y1 && a.y2==b.y2);
    assert(c.x2 < a.x1); // independent touch targets
    lv_obj_t *glyph=lv_obj_get_child(output,0);
    lv_color_t dim=lv_obj_get_style_image_recolor(glyph,0);
    assert(lv_color_eq(dim,lv_color_make(150,150,150)));
    sonos_t fixture={.count=1,.rooms={{.id="RINCON_TEST",.coordinator="RINCON_TEST",.name="Test room"}}};
    // Empty seed refuses network calls; only test selection styling.
    assert(speaker_output_select(&fixture,"RINCON_TEST","",0,false));
    player_refresh_wireless_output();
    assert(lv_color_eq(lv_obj_get_style_image_recolor(glyph,0),theme()->text_primary));
    speaker_output_local();player_refresh_wireless_output();
    assert(lv_color_eq(lv_obj_get_style_image_recolor(glyph,0),dim));
    assert(!lv_obj_is_hidden(output));
    wifi_online=false;player_refresh_wireless_output();assert(lv_obj_is_hidden(output));
    wifi_online=true;player_refresh_wireless_output();assert(!lv_obj_is_hidden(output));
    save_snapshot(player_screen,"/tmp/sonix-wireless-player.ppm");
    lv_obj_send_event(output,LV_EVENT_CLICKED,NULL);
    assert(sheet_open && lv_obj_get_parent(page_content)==sheet_card);
    lv_anim_delete(sheet_card,sheet_y);lv_obj_set_y(sheet_card,sheet_top);
    settle();
    assert(discoveries>=1 && shown.count==4);
    assert(find_text(sheet_card,"Output"));
    assert(!find_text(sheet_card,"Discover rooms") && !find_text(sheet_card,"Refresh groups"));
#ifdef HOST_BUILD
    assert(find_text(sheet_card,"Connect by speaker IP"));
#else
    assert(!find_text(sheet_card,"Connect by speaker IP"));
#endif
    assert(lv_obj_get_child_count(room_list)==3); // one group and two standalone rooms
    assert(!lv_obj_is_hidden(local_play_icon));
    lv_obj_t *group=lv_obj_get_child(room_list,0);
    lv_obj_t *group_header=lv_obj_get_child(group,0);
    lv_obj_send_event(group_header,LV_EVENT_CLICKED,NULL);settle();
    char active[SONOS_ID_MAX];speaker_output_room(active,sizeof(active));assert(!strcmp(active,"RINCON_A"));
    assert(lv_obj_is_hidden(local_play_icon));
    assert(widgets[0].slider && widgets[1].slider && !widgets[2].slider && !widgets[3].slider);
    assert(lv_slider_get_value(widgets[0].slider)==20 && lv_slider_get_value(widgets[1].slider)==35);
    // Inline slider addresses only Kitchen, with the final drag value retained.
    for(int value=40;value<=67;value++) {
        lv_slider_set_value(widgets[1].slider,value,LV_ANIM_OFF);
        lv_obj_send_event(widgets[1].slider,LV_EVENT_VALUE_CHANGED,NULL);
    }
    settle();
    pthread_mutex_lock(&mock_lock);
    assert(mock_volume[0]==20 && mock_volume[1]==67 && !strcmp(last_volume_room,"RINCON_B"));
    assert(volume_writes<28);pthread_mutex_unlock(&mock_lock); // coalescing limits HTTP work
    // Key/overlay volume uses the playback worker, not the picker's writer.
    pthread_mutex_lock(&mock_lock);block_group_volume=true;pthread_mutex_unlock(&mock_lock);
    set_volume_percent(44);
    for(int i=0;i<5000;i++) {
        gui_post_drain();
        if(lv_slider_get_value(widgets[0].slider)==44)break;
        usleep(1000);
    }
    assert(lv_slider_get_value(widgets[0].slider)==44);
    assert(lv_slider_get_value(widgets[1].slider)==91); // simultaneous preview, no speaker reply yet
    pthread_mutex_lock(&mock_lock);assert(mock_volume[0]==20 && mock_volume[1]==67);pthread_mutex_unlock(&mock_lock);
    // A partial/stale room read during the blocked write must not undo previews.
    pthread_mutex_lock(&lock);
    room_level_t *partial=level_for(snapshot_levels,"RINCON_B");
    partial->known=true;partial->value=67;partial->stamp=volume_stamp();result_ready=true;
    pthread_mutex_unlock(&lock);consume_result(NULL);
    assert(lv_slider_get_value(widgets[0].slider)==44 && lv_slider_get_value(widgets[1].slider)==91);
    pthread_mutex_lock(&mock_lock);block_group_volume=false;pthread_mutex_unlock(&mock_lock);
    settle();
    assert(lv_slider_get_value(widgets[1].slider)==91); // both rooms rose by 24
    assert(mock_volume[2]==50 && mock_volume[3]==25); // other groups untouched
    // At the coordinator's ceiling, keys still raise a quieter member.
    pthread_mutex_lock(&mock_lock);mock_volume[0]=100;mock_volume[1]=40;pthread_mutex_unlock(&mock_lock);
    change_volume_percent(3);
    for(int i=0;i<5000;i++) {
        gui_post_drain();usleep(1000);
        if(widgets[1].slider && lv_slider_get_value(widgets[1].slider)==43)break;
    }
    settle();assert(lv_slider_get_value(widgets[0].slider)==100 && lv_slider_get_value(widgets[1].slider)==43);
    change_volume_percent(-5);
    for(int i=0;i<5000;i++) {
        gui_post_drain();usleep(1000);
        if(widgets[0].slider && lv_slider_get_value(widgets[0].slider)==95)break;
    }
    settle();assert(lv_slider_get_value(widgets[0].slider)==95 && lv_slider_get_value(widgets[1].slider)==38);
    // An older picker reply must not undo the newer playback-volume report.
    room_level_t *latest=level_for(levels,"RINCON_A");assert(latest && latest->value==95);
    pthread_mutex_lock(&lock);
    room_level_t *stale=level_for(snapshot_levels,"RINCON_A");
    stale->known=true;stale->value=20;stale->stamp=latest->stamp-1;result_ready=true;
    pthread_mutex_unlock(&lock);consume_result(NULL);
    assert(lv_slider_get_value(widgets[0].slider)==95);
    group=lv_obj_get_child(room_list,0);group_header=lv_obj_get_child(group,0);
    assert(!lv_obj_is_hidden(lv_obj_get_child(group_header,0))); // active group playback mark
    lv_obj_update_layout(sheet_card);save_snapshot(sheet_card,"/tmp/sonix-wireless-sheet.ppm");

    // Plus on standalone Office adds it to the current group without selecting it.
    lv_obj_t *office=lv_obj_get_child(room_list,1);
    lv_obj_t *plus=image_parent(office,&icon_plus);assert(plus);
    lv_obj_send_event(plus,LV_EVENT_CLICKED,NULL);settle();
    assert(!strcmp(last_join,"RINCON_C") && !strcmp(last_target,"RINCON_A"));
    assert(group_size(&shown,"RINCON_A")==3 && widgets[2].slider);
    // X removes one member.
    group=lv_obj_get_child(room_list,0);
    lv_obj_t *kitchen=lv_obj_get_child(group,2);
    lv_obj_t *remove=image_parent(kitchen,&icon_close);assert(remove);
    lv_obj_send_event(remove,LV_EVENT_CLICKED,NULL);settle();
    assert(!strcmp(last_leave,"RINCON_B") && group_size(&shown,"RINCON_A")==2);
    assert(!widgets[1].slider);
    // Removing the coordinator follows the remaining group, not that room.
    group=lv_obj_get_child(room_list,0);
    lv_obj_t *coordinator=lv_obj_get_child(group,1);
    remove=image_parent(coordinator,&icon_close);assert(remove);
    lv_obj_send_event(remove,LV_EVENT_CLICKED,NULL);settle();
    speaker_output_room(active,sizeof(active));assert(!strcmp(active,"RINCON_C"));
    assert(widgets[2].slider && !widgets[0].slider);
    local_output_cb(NULL);settle();assert(!speaker_output_active() && !lv_obj_is_hidden(local_play_icon));
    for(int i=0;i<shown.count;i++)assert(!widgets[i].slider);
    // Reopening refreshes automatically; local selection isn't changed.
    int before=refreshes;
    sheet_close();assert(!sheet_open);
    lv_anim_delete(sheet_card,sheet_y);sheet_hidden(NULL);
    assert(lv_obj_get_parent(page_content)==speakers_screen && lv_obj_is_hidden(sheet_veil));
    speakers_output_open();sheet_close();speakers_output_open();
    assert(sheet_open && lv_obj_get_parent(page_content)==sheet_card);
    settle();assert(refreshes>before && !speaker_output_active());
    theme_set_accent(4);theme_set_dynamic_tint(true);theme_set_dark(!theme_is_dark());
    puts("Wireless sheet layout and lifecycle tests passed");return 0;
}
