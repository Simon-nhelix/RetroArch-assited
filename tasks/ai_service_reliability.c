#include <string.h>

#include "ai_service_reliability.h"

bool ai_service_error_is_transient(const char *error)
{
   if (!error || !*error)
      return false;
   if (strcmp(error, "No text found.") == 0)
      return false;
   if (   strstr(error, "Select a model")
       || strstr(error, "AI Service URL")
       || strstr(error, "requires macOS")
       || strstr(error, "Apple Vision OCR requires"))
      return false;
   return true;
}

int64_t ai_service_retry_delay_usec(unsigned error_streak)
{
   unsigned shift = error_streak > 0 ? error_streak - 1 : 0;
   int64_t delay;
   if (shift > 4)
      shift = 4;
   delay = (int64_t)1000000 << shift;
   if (delay > AI_SERVICE_RETRY_MAX_USEC)
      delay = AI_SERVICE_RETRY_MAX_USEC;
   return delay;
}

bool ai_service_request_timed_out(int64_t started_usec, int64_t now_usec,
      unsigned timeout_sec)
{
   int64_t limit;
   if (timeout_sec == 0)
      return false;
   if (now_usec < started_usec)
      return false;
   limit = (int64_t)timeout_sec * 1000000;
   return (now_usec - started_usec) >= limit;
}
