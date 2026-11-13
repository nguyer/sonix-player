#include "subsonicofflinepage.h"
#include "subsonicpage.h"
#include "src/system/library/libraryoffline.h"
#include "src/system/streaming/subsoniccache.h"
#include "src/system/streaming/subsonicoffline.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/fonts/fonts.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static lv_obj_t *screen,*content;
static subsonic_client_t *client;
static char source[33];
static void rebuild(void);
static bool account_source(void){if(!client || !client->base_url[0])return false;subsoniccache_source(client,source);return true;}
static void save(offline_options_t *o){if(!library_offline_save_options(source,o))gui_notify_popup("Cannot save offline settings while the library is busy.");rebuild();}

static lv_obj_t *slider_labels[3];
static const int storage_values[]={1,2,4,8,16,32,64,128,256,0};
static const int reserve_values[]={128,256,512,1024,2048,4096,8192};
static void selection_cb(lv_event_t *e){
 if(switcher_back_drag_active() || !account_source())return;
 offline_options_t o;if(!library_offline_options(source,&o))return;
 int choice=(int)(intptr_t)lv_event_get_user_data(e);
 if(choice<3){o.mode=choice;if(o.mode==OFFLINE_OFF)subsonicoffline_cancel();}
 subsonicpage_downloads_changed();save(&o);
}
static void toggle_cb(lv_event_t *e){
 if(switcher_back_drag_active() || !account_source())return;
 offline_options_t o;if(!library_offline_options(source,&o))return;
 lv_obj_t *toggle=lv_event_get_target(e);
 bool enabled=lv_obj_has_state(toggle,LV_STATE_CHECKED);
 o.auto_cache=enabled;
 if(!library_offline_save_options(source,&o))gui_notify_popup("Cannot save offline settings while the library is busy.");
}
static void slider_text(int which,int value){
 char text[64];
 if(which==1){if(storage_values[value])snprintf(text,sizeof(text),"%d GiB",storage_values[value]);else strcpy(text,"Unlimited");}
 else snprintf(text,sizeof(text),"%d MiB",reserve_values[value]);
 lv_label_set_text(slider_labels[which],text);
}
static void slider_cb(lv_event_t *e){
 lv_event_code_t code=lv_event_get_code(e);
 if(code!=LV_EVENT_VALUE_CHANGED && code!=LV_EVENT_RELEASED)return;
 lv_obj_t *slider=lv_event_get_target(e);
 int which=(int)(intptr_t)lv_event_get_user_data(e),value=lv_slider_get_value(slider);
 slider_text(which,value);
 if(code!=LV_EVENT_RELEASED || switcher_back_drag_active() || !account_source())return;
 offline_options_t o;if(!library_offline_options(source,&o))return;
 if(which==1)o.max_bytes=(int64_t)storage_values[value]*1024*1024*1024;
 if(which==2)o.reserve_bytes=(int64_t)reserve_values[value]*1024*1024;
 if(!library_offline_save_options(source,&o))gui_notify_popup("Cannot save offline settings while the library is busy.");
 if(which==1)rebuild();
}
static void slider_control(const char *name,int which,int64_t value){
 const int *values=which==1?storage_values:reserve_values;
 int count=which==1?(int)(sizeof(storage_values)/sizeof(storage_values[0])):(int)(sizeof(reserve_values)/sizeof(reserve_values[0])),index=0;
 int64_t difference=INT64_MAX;
 for(int i=0;i<count;++i){int64_t d=value-values[i];if(d<0)d=-d;if(d<difference){difference=d;index=i;}}
 lv_obj_t *slider;settingsrow_slider(content,name,count,&slider_labels[which],&slider,NULL);
 lv_slider_set_value(slider,index,LV_ANIM_OFF);slider_text(which,index);
 lv_obj_add_event_cb(slider,slider_cb,LV_EVENT_ALL,(void *)(intptr_t)which);
}
static void toggle_control(const char *name,bool enabled,int which){
 lv_obj_t *toggle;lv_obj_t *card=settingsrow_toggle(content,name,&toggle,toggle_cb);
 if(which==1){lv_obj_t *label=lv_obj_get_child(card,0);lv_label_set_long_mode(label,LV_LABEL_LONG_WRAP);lv_obj_set_height(label,LV_SIZE_CONTENT);lv_obj_set_height(card,110);}
 lv_obj_set_user_data(toggle,(void *)(intptr_t)which);
 if(enabled)lv_obj_add_state(toggle,LV_STATE_CHECKED);
}
static void remove_all_cb(lv_event_t *e){(void)e;if(!switcher_back_drag_active())subsonicpage_remove_all_downloads();}
static void stop_cb(lv_event_t *e){(void)e;subsonicoffline_cancel();}
static void build_content(void){
 lv_obj_clean(content);
 if(!account_source()){settingsrow_heading(content,"Connect to Navidrome to configure offline music.");return;}
 offline_options_t o;if(!library_offline_options(source,&o)){settingsrow_heading(content,"Music storage is unavailable.");return;}
 subsonic_offline_status_t state;subsonicoffline_status(&state);
 const char *modes[]={"Off","Selective","Full library"};
 lv_obj_t *pills;settingsrow_pills(content,"Offline mode",&pills);
 for(int i=1;i<3;++i){lv_obj_t *pill=settingsrow_pill_text(pills,modes[i],i,selection_cb);settingsrow_pill_active(pill,o.mode==i);}
 if(o.mode==OFFLINE_SELECTIVE)toggle_control("Automatically cache recently played",o.auto_cache,1);
 slider_control("Maximum offline storage",1,o.max_bytes/(1024LL*1024*1024));
 if(!o.max_bytes)slider_control("Reserve free space",2,o.reserve_bytes/(1024LL*1024));
 lv_obj_t *remove=lv_button_create(content);lv_obj_set_size(remove,LV_PCT(100),64);
 lv_obj_add_style(remove,&theme_style_card,0);
 lv_obj_set_style_bg_color(remove,lv_color_hex(0xc01c28),0);
 lv_obj_set_style_radius(remove,LV_RADIUS_CIRCLE,0);lv_obj_set_style_shadow_width(remove,0,0);
 lv_obj_add_event_cb(remove,remove_all_cb,LV_EVENT_CLICKED,NULL);
 lv_obj_t *label=lv_label_create(remove);lv_label_set_text(label,"Remove all downloads");
 lv_obj_set_style_text_font(label,&font_ui_24,0);lv_obj_set_style_text_color(label,lv_color_white(),0);lv_obj_center(label);
 if(state.running && !state.removing)settingsrow_action(content,"Stop offline sync",stop_cb,NULL);
}
static void rebuild(void){
 int y=lv_obj_get_scroll_y(content);
 build_content();lv_obj_update_layout(content);
 lv_obj_scroll_to_y(content,y,LV_ANIM_OFF);
}
static void loaded(lv_event_t *e){(void)e;rebuild();}
void subsonicofflinepage_notify(void){
 if(!screen || lv_screen_active()!=screen)return;
 subsonic_offline_status_t state;subsonicoffline_status(&state);
 static bool was_running,was_removing;
 if(was_running!=state.running || was_removing!=state.removing){was_running=state.running;was_removing=state.removing;rebuild();}
}
void subsonicofflinepage_open(void){rebuild();switch_screen(screen);}
void subsonicofflinepage_init(gui_config_t *cfg,subsonic_client_t *account){
 client=account;
 screen=lv_obj_create(NULL);content=settingsrow_page(screen,cfg,"Offline / cache settings");switcher_attach_back_gesture(screen);lv_obj_add_event_cb(screen,loaded,LV_EVENT_SCREEN_LOADED,NULL);
}

void subsonicofflinepage_refresh(void){if(screen && lv_screen_active()==screen)rebuild();}
