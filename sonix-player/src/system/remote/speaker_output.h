#ifndef SPEAKER_OUTPUT_H
#define SPEAKER_OUTPUT_H
#include "sonos.h"
#include "src/system/audio/audio.h"
// All entry points are nonblocking; the backend owns its LAN worker.
bool speaker_output_select(const sonos_t *rooms, const char *id, const char *path, double position, bool playing);
void speaker_output_local(void);
bool speaker_output_active(void);
// Change notification executes on the worker; callbacks must only post to the GUI.
void speaker_output_set_changed_cb(void (*callback)(void));
void speaker_output_set_completion_cb(void (*callback)(void));
void speaker_output_room(char *out, size_t cap);
// Follow a new coordinator of the same playing group without publishing a new
// track or stopping/restarting transport.
bool speaker_output_follow(const sonos_t *rooms, const char *id);
void speaker_output_name(char *out, size_t cap);
void speaker_output_error(char *out, size_t cap);
void speaker_output_play(const char *path, double position, bool paused);
void speaker_output_transport(audio_status_t status);
void speaker_output_seek(double position);
audio_status_t speaker_output_status(void);
void speaker_output_file(char *out, size_t cap);
void speaker_output_progress(double *position, double *duration);
bool speaker_output_take_completion(void);
int speaker_output_volume(void);
// Last confirmed room volume, with a monotonic timestamp for merging UI replies.
bool speaker_output_volume_feedback(char *room, size_t cap, int *percent, double *stamp);
void speaker_output_set_volume(int percent);
void speaker_output_change_volume(int delta);
unsigned speaker_output_group_volume_revision(void);
bool speaker_output_group_volume_pending(void);
// Called immediately on user input; callback must post UI work, not touch LVGL.
void speaker_output_set_volume_preview_cb(void (*cb)(const char *room, int value, bool absolute, double stamp));
#endif
