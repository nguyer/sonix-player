#include "sonos.h"
#include "src/system/net/http.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define XML_LIMIT (256 * 1024)
#define AV "AVTransport"
#define ZONE "ZoneGroupTopology"

static bool fail(sonos_t *s, const char *why) {
	snprintf(s->error, sizeof(s->error), "%s", why);
	return false;
}

// Decode one XML layer only: SOAP's ZoneGroupState is itself escaped XML.
static bool decode(const char *p, size_t n, char *out, size_t cap) {
	size_t used = 0;
	for (size_t i = 0; i < n; i++) {
		unsigned cp = (unsigned char)p[i];
		if (cp == '&') {
			const char *end = memchr(p + i, ';', n - i);
			if (!end || end - (p + i) > 12) return false;
			size_t len = (size_t)(end - p - i + 1);
			if (len == 5 && !memcmp(p + i, "&amp;", len)) cp = '&';
			else if (len == 4 && !memcmp(p + i, "&lt;", len)) cp = '<';
			else if (len == 4 && !memcmp(p + i, "&gt;", len)) cp = '>';
			else if (len == 6 && !memcmp(p + i, "&quot;", len)) cp = '"';
			else if (len == 6 && !memcmp(p + i, "&apos;", len)) cp = '\'';
			else if (p[i + 1] == '#') {
				char number[16], *tail;
				memcpy(number, p + i + 2, len - 3); number[len - 3] = 0;
				bool hex = number[0] == 'x';
				for (const char *digit = number + hex; *digit; digit++) {
					if (hex ? !isxdigit((unsigned char)*digit) : !isdigit((unsigned char)*digit)) return false;
				}
				unsigned long value = strtoul(number + hex, &tail, hex ? 16 : 10);
				if (*tail || tail == number + hex || value > 0x10ffff || value == 0 ||
					(value >= 0xd800 && value <= 0xdfff)) return false;
				cp = (unsigned)value;
			} else return false;
			i += len - 1;
			unsigned char bytes[4]; int count;
			if (cp < 0x80) { bytes[0] = cp; count = 1; }
			else if (cp < 0x800) { bytes[0] = 0xc0 | (cp >> 6); bytes[1] = 0x80 | (cp & 63); count = 2; }
			else if (cp < 0x10000) {
				bytes[0] = 0xe0 | (cp >> 12); bytes[1] = 0x80 | ((cp >> 6) & 63);
				bytes[2] = 0x80 | (cp & 63); count = 3;
			} else {
				bytes[0] = 0xf0 | (cp >> 18); bytes[1] = 0x80 | ((cp >> 12) & 63);
				bytes[2] = 0x80 | ((cp >> 6) & 63); bytes[3] = 0x80 | (cp & 63); count = 4;
			}
			if (used + count >= cap) return false;
			memcpy(out + used, bytes, count); used += count;
		} else {
			if (!cp || used + 1 >= cap) return false;
			out[used++] = cp;
		}
	}
	out[used] = 0;
	return true;
}

// Attribute names must start at a token boundary; text inside another value
// cannot masquerade as an attribute. The end points at the tag's closing >.
static bool attribute(const char *p, const char *end, const char *key, char *out, size_t cap) {
	while (p < end && !isspace((unsigned char)*p) && *p != '>') p++;
	while (p < end) {
		while (p < end && isspace((unsigned char)*p)) p++;
		if (p == end || *p == '/') break;
		const char *name = p;
		while (p < end && *p != '=' && !isspace((unsigned char)*p)) p++;
		size_t len = p - name;
		while (p < end && isspace((unsigned char)*p)) p++;
		if (p == end || *p++ != '=') return false;
		while (p < end && isspace((unsigned char)*p)) p++;
		if (p == end || (*p != '\'' && *p != '"')) return false;
		char quote = *p++;
		const char *value = p;
		while (p < end && *p != quote) p++;
		if (p == end) return false;
		if (strlen(key) == len && !memcmp(name, key, len)) return decode(value, p - value, out, cap);
		p++;
	}
	return false;
}

static bool id_valid(const char *id) {
	if (strncmp(id, "RINCON_", 7) || !id[7]) return false;
	for (const char *p = id; *p; p++) if (!isalnum((unsigned char)*p) && *p != '_') return false;
	return true;
}

static bool speaker_ip_valid(const char *ip) {
	struct in_addr address;
	if (!ip || inet_pton(AF_INET, ip, &address) != 1) return false;
	unsigned host = ntohl(address.s_addr);
	return (host >> 24) != 0 && (host >> 24) != 127 && (host >> 28) < 14;
}

static bool location_ip(const char *url, char ip[16]) {
	int n = 0;
	if (sscanf(url, "http://%15[0-9.]:1400/%n", ip, &n) != 1 || !n) return false;
	return speaker_ip_valid(ip);
}

bool sonos_parse_topology(sonos_t *s, const char *xml) {
	s->count = 0;
	if (!xml || strlen(xml) > XML_LIMIT || !strstr(xml, "</ZoneGroups>"))
		return fail(s, "Incomplete Sonos room list");
	sonos_room_t rooms[SONOS_MAX_ROOMS]; int count = 0;
	const char *group = xml;
	while ((group = strstr(group, "<ZoneGroup ")) != NULL) {
		const char *end = strchr(group, '>'), *close = strstr(group, "</ZoneGroup>");
		char coordinator[SONOS_ID_MAX];
		if (!end || !close || end >= close || !attribute(group, end, "Coordinator", coordinator, sizeof(coordinator)) ||
			!id_valid(coordinator)) return fail(s, "Invalid Sonos group");
		const char *member = end;
		while ((member = strstr(member, "<ZoneGroupMember ")) != NULL && member < close) {
			end = strchr(member, '>');
			if (!end || end >= close) return fail(s, "Incomplete Sonos room");
			char invisible[8] = "";
			attribute(member, end, "Invisible", invisible, sizeof(invisible));
			if (strcmp(invisible, "1")) {
				if (count == SONOS_MAX_ROOMS) return fail(s, "Too many Sonos rooms");
				sonos_room_t *r = &rooms[count]; memset(r, 0, sizeof(*r));
				char location[256];
				if (!attribute(member, end, "UUID", r->id, sizeof(r->id)) || !id_valid(r->id) ||
					!attribute(member, end, "ZoneName", r->name, sizeof(r->name)) ||
					!attribute(member, end, "Location", location, sizeof(location)) || !location_ip(location, r->ip))
					return fail(s, "Invalid Sonos room description");
				for (int i = 0; i < count; i++) if (!strcmp(rooms[i].id, r->id)) return fail(s, "Duplicate Sonos room");
				snprintf(r->coordinator, sizeof(r->coordinator), "%s", coordinator); count++;
			}
			member = end + 1;
		}
		group = close + strlen("</ZoneGroup>");
	}
	if (!count) return fail(s, "No visible Sonos rooms found");
	for (int i = 0; i < count; i++) {
		bool found = false;
		for (int j = 0; j < count; j++) if (!strcmp(rooms[i].coordinator, rooms[j].id)) found = true;
		if (!found) return fail(s, "Sonos group coordinator is unavailable");
	}
	memcpy(s->rooms, rooms, count * sizeof(*rooms)); s->count = count; s->error[0] = 0;
	return true;
}

static bool soap(sonos_t *s, const char *ip, const char *service, const char *action, const char *args, char **out) {
	char url[192], headers[256];
	const char *path = !strcmp(service, ZONE) ? "ZoneGroupTopology" :
		!strcmp(service, "RenderingControl") ? "MediaRenderer/RenderingControl" : "MediaRenderer/AVTransport";
	snprintf(url, sizeof(url), "http://%s:1400/%s/Control", ip, path);
	snprintf(headers, sizeof(headers), "SOAPACTION: \"urn:schemas-upnp-org:service:%s:1#%s\"\r\n", service, action);
	size_t size = strlen(args) + 512;
	char *body = malloc(size);
	if (!body) return fail(s, "Out of memory");
	snprintf(body, size, "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
		"s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s "
		"xmlns:u=\"urn:schemas-upnp-org:service:%s:1\">%s</u:%s></s:Body></s:Envelope>", action, service, args, action);
	http_req_t req = {.method = "POST", .extra_headers = headers, .body = body,
		.content_type = "text/xml; charset=\"utf-8\"", .want_error_body = true};
	char *response = NULL; int status = 0; size_t length = 0;
	bool ok = http_request(url, &req, &response, &length, XML_LIMIT, 3, &status);
	free(body);
	char expected[96]; snprintf(expected, sizeof(expected), "%sResponse", action);
	if (!ok || status != 200 || length >= XML_LIMIT || !response || strstr(response, "<s:Fault") ||
		strstr(response, "<SOAP-ENV:Fault") || !strstr(response, expected)) {
		free(response);
		snprintf(s->error, sizeof(s->error), "Sonos %s failed (HTTP %d). Refresh rooms and retry.", action, status);
		return false;
	}
	if (out) *out = response; else free(response);
	s->error[0] = 0; return true;
}

bool sonos_refresh(sonos_t *s) {
	if (!s->seed_ip[0]) return fail(s, "Discover Sonos rooms first");
	char *response = NULL;
	if (!soap(s, s->seed_ip, ZONE, "GetZoneGroupState", "", &response)) { s->count = 0; return false; }
	const char *start = strstr(response, "<ZoneGroupState>");
	const char *end = start ? strstr(start, "</ZoneGroupState>") : NULL;
	if (!end) { free(response); s->count = 0; return fail(s, "Missing Sonos room list"); }
	start += strlen("<ZoneGroupState>");
	char *xml = malloc((size_t)(end - start) + 1);
	bool ok = false;
	if (xml) {
		if (*start == '<') { memcpy(xml, start, end - start); xml[end - start] = 0; ok = true; }
		else ok = decode(start, end - start, xml, (size_t)(end - start) + 1);
		if (ok) ok = sonos_parse_topology(s, xml);
		else { s->count = 0; fail(s, "Invalid Sonos room XML"); }
		free(xml);
	} else { s->count = 0; fail(s, "Out of memory"); }
	free(response); return ok;
}

bool sonos_connect(sonos_t *s, const char *ip) {
	if (!speaker_ip_valid(ip)) return fail(s, "Enter a speaker IPv4 address, for example 192.168.1.50 (no port or URL)");
	// Copy first: callers may pass the context's current seed address.
	char address[16]; snprintf(address, sizeof(address), "%s", ip);
	snprintf(s->seed_ip, sizeof(s->seed_ip), "%s", address);
	return sonos_refresh(s);
}

bool sonos_discover(sonos_t *s) {
	s->count = 0; s->seed_ip[0] = 0;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) return fail(s, "Cannot open Sonos discovery socket");
	struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(1900)};
	inet_pton(AF_INET, "239.255.255.250", &addr.sin_addr);
	const char request[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
		"MAN: \"ssdp:discover\"\r\nMX: 1\r\nST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n";
	if (sendto(fd, request, sizeof(request) - 1, 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd); return fail(s, "Sonos discovery failed. Check Wi-Fi.");
	}
	struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
	long long deadline = (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000 + 2200;
	char candidate[16] = "";
	while (true) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		int left = (int)(deadline - ((long long)now.tv_sec * 1000 + now.tv_nsec / 1000000));
		struct pollfd pfd = {.fd = fd, .events = POLLIN};
		if (left <= 0 || poll(&pfd, 1, left) <= 0) break;
		char packet[4096]; struct sockaddr_in peer; socklen_t len = sizeof(peer);
		ssize_t n = recvfrom(fd, packet, sizeof(packet) - 1, 0, (struct sockaddr *)&peer, &len);
		if (n <= 0) continue;
		packet[n] = 0;
		if (strncmp(packet, "HTTP/1.1 200", 12) || !strstr(packet, "ZonePlayer:1")) continue;
		inet_ntop(AF_INET, &peer.sin_addr, candidate, sizeof(candidate)); break;
	}
	close(fd);
	if (!candidate[0]) return fail(s, "No Sonos rooms found. Use the same Wi-Fi network.");
	snprintf(s->seed_ip, sizeof(s->seed_ip), "%s", candidate);
	return sonos_refresh(s);
}

static sonos_room_t *room(sonos_t *s, const char *id) {
	for (int i = 0; i < s->count; i++) if (!strcmp(s->rooms[i].id, id)) return &s->rooms[i];
	fail(s, "Room is no longer available. Discover rooms again."); return NULL;
}

static sonos_room_t *coordinator(sonos_t *s, const char *id) {
	sonos_room_t *r = room(s, id); return r ? room(s, r->coordinator) : NULL;
}

bool sonos_join(sonos_t *s, const char *id, const char *target) {
	if (!sonos_refresh(s)) return false;
	sonos_room_t *r = room(s, id), *c = coordinator(s, target);
	if (!r || !c) return false;
	if (!strcmp(r->coordinator, c->id)) return true;
	// Moving a coordinator with followers can stop those followers. Make the
	// user detach it first, with the explicit ungroup action.
	for (int i = 0; i < s->count; i++) if (strcmp(s->rooms[i].id, r->id) && !strcmp(s->rooms[i].coordinator, r->id))
		return fail(s, "Ungroup this room before moving it to another group");
	char args[320];
	snprintf(args, sizeof(args), "<InstanceID>0</InstanceID><CurrentURI>x-rincon:%s</CurrentURI>"
		"<CurrentURIMetaData></CurrentURIMetaData>", c->id);
	if (!soap(s, r->ip, AV, "SetAVTransportURI", args, NULL)) return false;
	return sonos_refresh(s);
}

bool sonos_leave(sonos_t *s, const char *id) {
	if (!sonos_refresh(s)) return false;
	sonos_room_t *r = room(s, id); if (!r) return false;
	if (!soap(s, r->ip, AV, "BecomeCoordinatorOfStandaloneGroup", "<InstanceID>0</InstanceID>", NULL)) return false;
	return sonos_refresh(s);
}

bool sonos_transport(sonos_t *s, const char *id, const char *action) {
	if (strcmp(action, "Play") && strcmp(action, "Pause") && strcmp(action, "Stop")) return fail(s, "Unsupported speaker action");
	if (!sonos_refresh(s)) return false;
	sonos_room_t *c = coordinator(s, id); if (!c) return false;
	return soap(s, c->ip, AV, action, !strcmp(action, "Play") ?
		"<InstanceID>0</InstanceID><Speed>1</Speed>" : "<InstanceID>0</InstanceID>", NULL);
}

bool sonos_volume(sonos_t *s, const char *id, int percent) {
	if (percent < 0 || percent > 100) return fail(s, "Invalid speaker volume");
	if (!sonos_refresh(s)) return false;
	sonos_room_t *r = room(s, id); if (!r) return false;
	char args[192]; snprintf(args, sizeof(args), "<InstanceID>0</InstanceID><Channel>Master</Channel>"
		"<DesiredVolume>%d</DesiredVolume>", percent);
	return soap(s, r->ip, "RenderingControl", "SetVolume", args, NULL);
}

bool sonos_load_uri(sonos_t *s, const char *id, const char *uri, bool play) {
	if (!uri || strncmp(uri, "http://", 7) || strlen(uri) > 2048) return fail(s, "Invalid speaker track URL");
	if (!sonos_refresh(s)) return false;
	sonos_room_t *c = coordinator(s, id); if (!c) return false;
	char escaped[12289]; size_t used = 0;
	for (const unsigned char *p = (const unsigned char *)uri; *p; p++) {
		const char *entity = *p == '&' ? "&amp;" : *p == '<' ? "&lt;" : *p == '>' ? "&gt;" :
			*p == '"' ? "&quot;" : *p == '\'' ? "&apos;" : NULL;
		if (*p < 32 || *p == 127) return fail(s, "Invalid speaker track URL");
		if (entity) { size_t n = strlen(entity); memcpy(escaped + used, entity, n); used += n; }
		else escaped[used++] = *p;
	}
	escaped[used] = 0;
	char *args = malloc(used + 160); if (!args) return fail(s, "Out of memory");
	snprintf(args, used + 160, "<InstanceID>0</InstanceID><CurrentURI>%s</CurrentURI>"
		"<CurrentURIMetaData></CurrentURIMetaData>", escaped);
	bool ok = soap(s, c->ip, AV, "SetAVTransportURI", args, NULL); free(args);
	return ok && (!play || soap(s, c->ip, AV, "Play", "<InstanceID>0</InstanceID><Speed>1</Speed>", NULL));
}

static bool field(const char *xml, const char *name, char *out, size_t cap) {
    char open[80], close[80];
    snprintf(open, sizeof(open), "<%s>", name); snprintf(close, sizeof(close), "</%s>", name);
    const char *p = strstr(xml, open), *end;
    if (!p || !(end = strstr(p += strlen(open), close))) return false;
    return decode(p, end - p, out, cap);
}
bool sonos_get_room_volume(sonos_t *s, const char *id, int *percent) {
    sonos_room_t *r=room(s,id);
    if(!r || !percent)return false;
    char *xml=NULL, text[32];
    if(!soap(s,r->ip,"RenderingControl","GetVolume",
        "<InstanceID>0</InstanceID><Channel>Master</Channel>",&xml))return false;
    bool ok=field(xml,"CurrentVolume",text,sizeof(text));
    char *end=NULL;long value=ok ? strtol(text,&end,10) : -1;
    ok=ok && end!=text && !*end && value>=0 && value<=100;
    free(xml);
    if(!ok)return fail(s,"Invalid speaker volume");
    *percent=value;return true;
}

static double seconds_value(const char *text) {
    unsigned h, m; double sec; char extra;
    if (sscanf(text, "%u:%u:%lf%c", &h, &m, &sec, &extra) != 3 || h > 9999 || m > 59 || !(sec >= 0 && sec < 60)) return 0;
    return h * 3600.0 + m * 60.0 + sec;
}
bool sonos_seek(sonos_t *s, const char *id, double seconds) {
    if (!(seconds >= 0 && seconds < 36000000)) return fail(s, "Invalid speaker seek position");
    if (!sonos_refresh(s)) return false;
    sonos_room_t *c = coordinator(s, id); if (!c) return false;
    unsigned n = (unsigned)seconds;
    char args[256];
    snprintf(args, sizeof(args), "<InstanceID>0</InstanceID><Unit>REL_TIME</Unit><Target>%02u:%02u:%02u</Target>",
        n / 3600, n / 60 % 60, n % 60);
    return soap(s, c->ip, AV, "Seek", args, NULL);
}
bool sonos_get_playback(sonos_t *s, const char *id, sonos_playback_t *out) {
    sonos_playback_t value = {0}; char *xml = NULL, text[64];
    if (!out || !sonos_refresh(s)) return false;
    sonos_room_t *c = coordinator(s, id), *r = room(s, id);
    if (!c || !r) return false;
    if (!soap(s, c->ip, AV, "GetTransportInfo", "<InstanceID>0</InstanceID>", &xml)) return false;
    bool ok = field(xml, "CurrentTransportState", value.state, sizeof(value.state));
    free(xml); xml = NULL;
    if (!ok) return fail(s, "Invalid speaker playback state");
    if (!soap(s, c->ip, AV, "GetPositionInfo", "<InstanceID>0</InstanceID>", &xml)) return false;
    struct timespec sample; clock_gettime(CLOCK_MONOTONIC,&sample);
    value.position_sample=sample.tv_sec+sample.tv_nsec/1000000000.0;
    ok = field(xml, "TrackURI", value.uri, sizeof(value.uri));
    if (field(xml, "RelTime", text, sizeof(text))) value.position = seconds_value(text);
    if (field(xml, "TrackDuration", text, sizeof(text))) value.duration = seconds_value(text);
    free(xml); xml = NULL;
    if (!ok) return fail(s, "Invalid speaker track information");
    if (!soap(s, r->ip, "RenderingControl", "GetVolume",
        "<InstanceID>0</InstanceID><Channel>Master</Channel>", &xml)) return false;
    ok = field(xml, "CurrentVolume", text, sizeof(text));
    char *end = NULL; long volume = ok ? strtol(text, &end, 10) : -1;
    ok = ok && end != text && !*end && volume >= 0 && volume <= 100;
    free(xml);
    if (!ok) return fail(s, "Invalid speaker volume");
    value.volume = volume; *out = value; return true;
}

bool sonos_play_uri(sonos_t *s, const char *id, const char *uri) {
    return sonos_load_uri(s,id,uri,true);
}

bool sonos_group_volume(sonos_t *s,const char *id,int value,bool absolute) {
    if(value < -10000 || value > 10000 || (absolute && (value<0 || value>100)))
        return fail(s,"Invalid group volume adjustment");
    if(!sonos_refresh(s))return false;
    sonos_room_t *r=room(s,id);if(!r)return false;
    int delta=value;
    if(absolute) {
        int current;
        if(!sonos_get_room_volume(s,id,&current))return false;
        delta=value-current;
    }
    if(!delta)return true;
    char args[192];
    snprintf(args,sizeof(args),"<InstanceID>0</InstanceID><Channel>Master</Channel><Adjustment>%d</Adjustment>",delta);
    // SetRelativeVolume preserves each room's independent level and lets the
    // speaker clamp its own 0..100 bounds, without a read/modify/write race.
    bool ok=true;char first_error[192]="";
    for(int i=0;i<s->count;i++)if(!strcmp(s->rooms[i].coordinator,r->coordinator)) {
        if(!soap(s,s->rooms[i].ip,"RenderingControl","SetRelativeVolume",args,NULL)) {
            if(ok)snprintf(first_error,sizeof(first_error),"%s",s->error);
            ok=false;
        }
    }
    if(!ok)snprintf(s->error,sizeof(s->error),"%s",first_error);
    return ok;
}
