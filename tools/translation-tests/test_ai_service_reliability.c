#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../tasks/ai_service_reliability.h"

static int failures;

static void expect_true(const char *name, int cond)
{
   if (!cond)
   {
      fprintf(stderr, "FAIL %s\n", name);
      failures++;
   }
}

int main(void)
{
   expect_true("empty not transient",
         !ai_service_error_is_transient(NULL)
         && !ai_service_error_is_transient(""));
   expect_true("no-text not transient",
         !ai_service_error_is_transient("No text found."));
   expect_true("model missing not transient",
         !ai_service_error_is_transient(
               "Select a model in Settings > AI Service > Model."));
   expect_true("bad url not transient",
         !ai_service_error_is_transient(
               "Set a valid OpenAI-compatible endpoint in Settings > AI Service > AI Service URL."));
   expect_true("macos floor not transient",
         !ai_service_error_is_transient(
               "Apple Vision OCR requires macOS 10.15+ / iOS 13.0+."));
   expect_true("prepare/send is transient",
         ai_service_error_is_transient(
               "Could not prepare or send the AI translation request."));
   expect_true("ocr calloc message is transient",
         ai_service_error_is_transient(
               "Could not prepare the Apple Vision OCR request."));
   expect_true("ocr post message is transient",
         ai_service_error_is_transient(
               "Could not prepare or send the OCR translation request."));
   expect_true("capture is transient",
         ai_service_error_is_transient(AI_SERVICE_CAPTURE_ERROR));
   expect_true("timeout is transient",
         ai_service_error_is_transient(AI_SERVICE_TIMEOUT_ERROR));
   expect_true("http 5xx is transient",
         ai_service_error_is_transient(
               "OpenAI-compatible endpoint returned HTTP 503."));

   expect_true("streak 1 is 1s",
         ai_service_retry_delay_usec(1) == 1000000);
   expect_true("streak 2 is 2s",
         ai_service_retry_delay_usec(2) == 2000000);
   expect_true("streak 3 is 4s",
         ai_service_retry_delay_usec(3) == 4000000);
   expect_true("streak 4 is 8s",
         ai_service_retry_delay_usec(4) == 8000000);
   expect_true("streak 5 reaches 10s cap",
         ai_service_retry_delay_usec(5) == AI_SERVICE_RETRY_MAX_USEC);
   expect_true("streak 9 stays at 10s cap",
         ai_service_retry_delay_usec(9) == AI_SERVICE_RETRY_MAX_USEC);

   expect_true("timeout 0 never fires",
         !ai_service_request_timed_out(0, 60 * 1000000, 0));
   expect_true("29s of 30s not timed out",
         !ai_service_request_timed_out(0, 29 * 1000000, 30));
   expect_true("30s of 30s timed out",
         ai_service_request_timed_out(0, 30 * 1000000, 30));
   expect_true("clock skew not timed out",
         !ai_service_request_timed_out(50, 40, 30));

   if (failures)
   {
      fprintf(stderr, "%d failure(s)\n", failures);
      return 1;
   }
   puts("ok");
   return 0;
}
