#ifndef SUBSONICINTERNAL_H
#define SUBSONICINTERNAL_H
#include "subsonic.h"
#include "src/system/core/json.h"
// Success transfers body/document ownership to caller, as for the existing API.
bool subsonic_api_call(subsonic_client_t *client, const char *method, const char *params,
	char **body, json_doc_t *doc, int *response);
#endif
