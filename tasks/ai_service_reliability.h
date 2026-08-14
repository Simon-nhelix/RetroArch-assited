#ifndef _AI_SERVICE_RELIABILITY_H
#define _AI_SERVICE_RELIABILITY_H

#include <stdbool.h>
#include <stdint.h>

#define AI_SERVICE_RETRY_MAX_USEC (10 * 1000 * 1000)
#define AI_SERVICE_TIMEOUT_ERROR  "AI translation request timed out."
#define AI_SERVICE_CAPTURE_ERROR  "Could not capture a frame for translation."

bool ai_service_error_is_transient(const char *error);
int64_t ai_service_retry_delay_usec(unsigned error_streak);
bool ai_service_request_timed_out(int64_t started_usec, int64_t now_usec,
      unsigned timeout_sec);

#endif
