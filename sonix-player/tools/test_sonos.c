#include "src/system/remote/sonos.h"
#include "src/system/net/http.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A "RINCON_A1400"
#define B "RINCON_B1400"
#define C "RINCON_C1400"
static const char *topology =
	"<ZoneGroups><ZoneGroup Coordinator='" A "' ID='one'>"
	"<ZoneGroupMember UUID='" A "' ZoneName='Kitchen &amp; Dining' Location='http://192.168.1.2:1400/xml/device_description.xml'/>"
	"<ZoneGroupMember UUID='" B "' ZoneName='Bedroom' Location='http://192.168.1.3:1400/xml/device_description.xml'/>"
	"<ZoneGroupMember UUID='RINCON_HIDDEN' Invisible='1' ZoneName='Hidden'/></ZoneGroup>"
	"<ZoneGroup Coordinator='" C "' ID='two'><ZoneGroupMember UUID='" C "' ZoneName='Study &#xE9;' "
	"Location='http://192.168.1.4:1400/xml/device_description.xml'/></ZoneGroup></ZoneGroups>";
static char action[128], destination[256], arguments[16384];
static char mutation[128], mutation_destination[256], mutation_args[16384];
static int writes, requests, status = 200;
static bool network_ok = true, fault;

// Exercise actual SOAP construction and response parsing without a speaker.
bool http_request(const char *url, const http_req_t *req, char **out, size_t *len,
	size_t limit, int timeout, int *status_out) {
	(void)limit; assert(timeout > 0); assert(!strcmp(req->method, "POST"));
	requests++;
	assert(strstr(req->content_type, "text/xml"));
	const char *hash = strchr(req->extra_headers, '#'); assert(hash);
	size_t n = strcspn(hash + 1, "\""); assert(n < sizeof(action));
	memcpy(action, hash + 1, n); action[n] = 0;
	snprintf(destination, sizeof(destination), "%s", url);
	snprintf(arguments, sizeof(arguments), "%s", req->body);
	char *response = malloc(32768); assert(response);
	if (!strcmp(action, "GetZoneGroupState")) {
		// SOAP contains one escaped layer around the XML document.
		strcpy(response, "<s:Envelope><s:Body><u:GetZoneGroupStateResponse><ZoneGroupState>");
		for (const char *p = topology; *p; p++) {
			if (*p == '&') strcat(response, "&amp;");
			else if (*p == '<') strcat(response, "&lt;");
			else if (*p == '>') strcat(response, "&gt;");
			else { size_t used = strlen(response); response[used] = *p; response[used + 1] = 0; }
		}
		strcat(response, "</ZoneGroupState></u:GetZoneGroupStateResponse></s:Body></s:Envelope>");
	} else if (!strcmp(action,"GetTransportInfo")) {
        strcpy(response,"<u:GetTransportInfoResponse><CurrentTransportState>PAUSED_PLAYBACK</CurrentTransportState></u:GetTransportInfoResponse>");
    } else if (!strcmp(action,"GetPositionInfo")) {
        strcpy(response,"<u:GetPositionInfoResponse><TrackURI>http://host/track.mp3?a=1&amp;b=2</TrackURI><RelTime>01:02:03</RelTime><TrackDuration>01:04:05</TrackDuration></u:GetPositionInfoResponse>");
    } else if (!strcmp(action,"GetVolume")) {
        strcpy(response,"<u:GetVolumeResponse><CurrentVolume>37</CurrentVolume></u:GetVolumeResponse>");
    } else {
        writes++;
		snprintf(mutation, sizeof(mutation), "%s", action);
		snprintf(mutation_destination, sizeof(mutation_destination), "%s", url);
		snprintf(mutation_args, sizeof(mutation_args), "%s", req->body);
		if (fault) strcpy(response, "<s:Fault>Failed</s:Fault>");
		else snprintf(response, 32768, "<u:%sResponse/>", action);
	}
	*out = response; *len = strlen(response); *status_out = status;
	return network_ok;
}

int main(void) {
	sonos_t s = {0};
	assert(sonos_parse_topology(&s, topology)); assert(s.count == 3);
	assert(!strcmp(s.rooms[0].name, "Kitchen & Dining"));
	assert(!strcmp(s.rooms[2].name, "Study \xc3\xa9"));
	assert(!strcmp(s.rooms[1].coordinator, A));
	assert(!sonos_parse_topology(&s, "<ZoneGroups>")); assert(s.count == 0);
	assert(!sonos_parse_topology(&s,
		"<ZoneGroups><ZoneGroup Coordinator='RINCON_GONE'><ZoneGroupMember UUID='" A "' "
		"ZoneName='Room' Location='http://192.168.1.2:1400/xml'/></ZoneGroup></ZoneGroups>"));
	assert(s.count == 0);
	assert(!sonos_parse_topology(&s,
		"<ZoneGroups><ZoneGroup Coordinator='" A "'><ZoneGroupMember UUID='" A "' "
		"ZoneName='Room' Location='http://127.0.0.1:1400/xml'/></ZoneGroup></ZoneGroups>"));
	int request_count = requests;
	const char *invalid_ips[] = {"", "localhost", "http://192.168.1.2", "192.168.1.2:1400", "192.168.1.999",
		"127.0.0.1", "0.0.0.0", "0.1.2.3", "224.0.0.1", "255.255.255.255", "::1", "192.168.1.2\r\n"};
	for (size_t i = 0; i < sizeof(invalid_ips) / sizeof(invalid_ips[0]); i++) assert(!sonos_connect(&s, invalid_ips[i]));
	assert(!sonos_connect(&s, NULL)); assert(requests == request_count);
	assert(sonos_connect(&s, "192.168.1.2")); assert(s.count == 3);
	assert(!strcmp(s.seed_ip, "192.168.1.2"));
	assert(!strcmp(destination, "http://192.168.1.2:1400/ZoneGroupTopology/Control"));
	assert(sonos_connect(&s, s.seed_ip)); // safe when input aliases context storage
	request_count = requests;
	assert(!sonos_connect(&s, "not-an-ip")); assert(s.count == 3); assert(requests == request_count);
	assert(sonos_refresh(&s)); assert(s.count == 3);
	assert(sonos_transport(&s, B, "Pause"));
	assert(!strcmp(action, "Pause")); assert(strstr(destination, "192.168.1.2:1400/MediaRenderer/AVTransport/Control"));
	assert(sonos_volume(&s, B, 25)); assert(strstr(destination, "192.168.1.3:1400/MediaRenderer/RenderingControl/Control"));
	assert(strstr(arguments, "<DesiredVolume>25</DesiredVolume>"));
    int group_before=writes;
    assert(sonos_group_volume(&s,B,5,false));assert(writes==group_before+2);
    assert(strstr(arguments,"<Adjustment>5</Adjustment>")); // two members, same delta
    assert(strstr(destination,"192.168.1.3:1400"));
    group_before=writes;
    assert(sonos_group_volume(&s,B,42,true));assert(writes==group_before+2);
    assert(strstr(arguments,"<Adjustment>5</Adjustment>")); // selected room starts at 37
	int before = writes;
	assert(!sonos_volume(&s, B, 101)); assert(!sonos_transport(&s, B, "Delete")); assert(writes == before);
	assert(!sonos_join(&s, A, C)); assert(writes == before); // protects coordinator's followers
	assert(sonos_join(&s, B, A)); assert(writes == before); // already grouped, no mutation
	assert(sonos_join(&s, C, B)); // joins the target's coordinator
	assert(!strcmp(mutation, "SetAVTransportURI"));
	assert(strstr(mutation_destination, "192.168.1.4:1400"));
	assert(strstr(mutation_args, "<CurrentURI>x-rincon:" A "</CurrentURI>"));
	assert(!strcmp(action, "GetZoneGroupState"));
	assert(sonos_leave(&s, B));
	assert(!strcmp(mutation, "BecomeCoordinatorOfStandaloneGroup"));
	assert(strstr(mutation_destination, "192.168.1.3:1400"));
	assert(sonos_play_uri(&s, B, "http://192.168.1.10:8123/track/test.mp3?a=1&b=2"));
	assert(!strcmp(action, "Play")); assert(strstr(destination, "192.168.1.2:1400"));
	assert(!sonos_play_uri(&s, B, "file:///etc/passwd"));
	assert(!sonos_play_uri(&s, B, "http://host/\r\nInjected"));

    int room_volume=-1;
    assert(sonos_get_room_volume(&s,B,&room_volume) && room_volume==37);
    assert(strstr(destination,"192.168.1.3:1400/MediaRenderer/RenderingControl/Control"));
    sonos_playback_t playback;
    assert(sonos_get_playback(&s,B,&playback));
    assert(!strcmp(playback.state,"PAUSED_PLAYBACK"));
    assert(!strcmp(playback.uri,"http://host/track.mp3?a=1&b=2"));
    assert(playback.position==3723 && playback.duration==3845 && playback.volume==37);
    assert(strstr(destination,"192.168.1.3:1400")); // selected member volume
    assert(sonos_seek(&s,B,3723));
    assert(strstr(arguments,"<Unit>REL_TIME</Unit><Target>01:02:03</Target>"));
    assert(strstr(destination,"192.168.1.2:1400")); // coordinator transport
    assert(!sonos_seek(&s,B,-1));
    assert(sonos_load_uri(&s,B,"http://host/track.mp3",false));
    assert(!strcmp(action,"SetAVTransportURI")); // no audible burst when loading paused
	fault = true; assert(!sonos_transport(&s, B, "Play")); fault = false;
	status = 500; assert(!sonos_refresh(&s)); assert(!s.count); status = 200;
	network_ok = false; assert(!sonos_connect(&s, "192.168.1.50")); assert(!s.count);
	puts("Sonos protocol tests passed"); return 0;
}
