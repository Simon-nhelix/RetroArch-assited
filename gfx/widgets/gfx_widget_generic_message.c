/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2014-2017 - Jean-André Santoni
 *  Copyright (C) 2015-2018 - Andre Leiradella
 *  Copyright (C) 2018-2020 - natinusala
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */
#include <compat/strl.h>
#include <string/stdstring.h>

#include "../gfx_widgets.h"
#include "../gfx_animation.h"
#include "../gfx_display.h"

#ifdef HAVE_TRANSLATE
#include "../../translation_defines.h"
#endif

#define GENERIC_MESSAGE_FADE_DURATION MSG_QUEUE_ANIMATION_DURATION

#ifdef HAVE_TRANSLATE
#define AI_SERVICE_MESSAGE_SIZE      16384
#define AI_SERVICE_MESSAGE_MAX_LINES 8
#define AI_SERVICE_WRAPPED_MESSAGE_SIZE \
      (AI_SERVICE_MESSAGE_SIZE + AI_SERVICE_MESSAGE_MAX_LINES + 4)
#endif

/* Widget state */

enum gfx_widget_generic_message_status
{
   GFX_WIDGET_GENERIC_MESSAGE_IDLE = 0,
   GFX_WIDGET_GENERIC_MESSAGE_SLIDE_IN,
   GFX_WIDGET_GENERIC_MESSAGE_FADE_IN,
   GFX_WIDGET_GENERIC_MESSAGE_WAIT,
   GFX_WIDGET_GENERIC_MESSAGE_FADE_OUT
};

struct gfx_widget_generic_message_state
{
   unsigned bg_min_width;
   unsigned bg_width;
   unsigned bg_height;
   unsigned frame_width;
   unsigned text_padding;
   unsigned text_color;

   unsigned message_duration;

   float timer;   /* float alignment */

   float bg_x;
   float bg_y_start;
   float bg_y_end;
   float text_x;
   float text_y_start;
   float text_y_end;

   float alpha;

   float bg_color[16];
   float frame_color[16];

   size_t message_len;

   enum gfx_widget_generic_message_status status;

   char message[512];
   bool message_updated;
};

typedef struct gfx_widget_generic_message_state gfx_widget_generic_message_state_t;

#ifdef HAVE_TRANSLATE
struct gfx_widget_ai_service_message_state
{
   unsigned bg_width;
   unsigned bg_height;
   unsigned frame_width;
   unsigned text_padding;
   unsigned text_color;
   unsigned line_count;
   unsigned message_duration;
   unsigned target_language;

   float timer;
   float bg_x;
   float bg_y;
   float text_x;
   float text_y;

   size_t message_len;
   size_t wrapped_len;

   char message[AI_SERVICE_MESSAGE_SIZE];
   char wrapped[AI_SERVICE_WRAPPED_MESSAGE_SIZE];
   bool visible;
   bool message_updated;
};

typedef struct gfx_widget_ai_service_message_state
      gfx_widget_ai_service_message_state_t;
#endif

static gfx_widget_generic_message_state_t p_w_generic_message_st = {

   0,                                  /* bg_min_width */
   0,                                  /* bg_width */
   0,                                  /* bg_height */
   0,                                  /* frame_width */
   0,                                  /* text_padding */
   TEXT_COLOR_INFO,                    /* text_color */

   0,                                  /* message_duration */

   0.0f,                               /* timer */

   0.0f,                               /* bg_x */
   0.0f,                               /* bg_y_start */
   0.0f,                               /* bg_y_end */
   0.0f,                               /* text_x */
   0.0f,                               /* text_y_start */
   0.0f,                               /* text_y_end */

   0.0f,                               /* alpha */

   COLOR_HEX_TO_FLOAT(BG_COLOR_DEFAULT, 1.0f), /* bg_color */
   COLOR_HEX_TO_FLOAT(BG_COLOR_DEFAULT, 0.0f), /* frame_color */
  
   0,                                  /* message_len */

   GFX_WIDGET_GENERIC_MESSAGE_IDLE,    /* status */

   {'\0'},                             /* message */
   false                               /* message_updated */
};

#ifdef HAVE_TRANSLATE
static gfx_widget_ai_service_message_state_t p_w_ai_service_message_st = {
   0
};
#endif

/* Utilities */

/* strlcpy() returns the source length, which is not safe to pass as a
 * destination-buffer length when truncation occurred. Keep the actual copied
 * length and, when possible, avoid leaving a partial UTF-8 sequence at the
 * end of the bounded destination. Invalid input remains bounded and NUL
 * terminated; callers are still responsible for supplying a C string. */
static size_t gfx_widget_copy_message(
      char *dst, size_t dst_size, const char *src)
{
   size_t src_len;
   size_t copied;

   if (!dst || dst_size == 0)
      return 0;
   if (!src)
   {
      dst[0] = '\0';
      return 0;
   }

   src_len = strlcpy(dst, src, dst_size);
   copied  = src_len < dst_size ? src_len : dst_size - 1;

   if (src_len >= dst_size && copied > 0)
   {
      size_t sequence_start = copied - 1;
      size_t sequence_len;
      unsigned char lead;

      while (sequence_start > 0
            && (((unsigned char)dst[sequence_start] & 0xC0) == 0x80))
         sequence_start--;

      lead = (unsigned char)dst[sequence_start];
      if (lead < 0x80)
         sequence_len = 1;
      else if (lead >= 0xC2 && lead <= 0xDF)
         sequence_len = 2;
      else if (lead >= 0xE0 && lead <= 0xEF)
         sequence_len = 3;
      else if (lead >= 0xF0 && lead <= 0xF4)
         sequence_len = 4;
      else
         sequence_len = 1;

      if (copied - sequence_start < sequence_len)
      {
         dst[sequence_start] = '\0';
         copied              = sequence_start;
      }
   }

   return copied;
}

static void gfx_widget_generic_message_reset(bool cancel_pending)
{
   gfx_widget_generic_message_state_t *state = &p_w_generic_message_st;
   uintptr_t alpha_tag                       = (uintptr_t)&state->alpha;
   uintptr_t timer_tag                       = (uintptr_t)&state->timer;

   /* Kill any existing timers/animations */
   gfx_animation_kill_by_tag(&timer_tag);
   gfx_animation_kill_by_tag(&alpha_tag);

   /* Reset status */
   state->status             = GFX_WIDGET_GENERIC_MESSAGE_IDLE;
   if (cancel_pending)
      state->message_updated = false;
}

#ifdef HAVE_TRANSLATE
static void gfx_widget_ai_service_message_reset(bool cancel_pending)
{
   gfx_widget_ai_service_message_state_t *state =
         &p_w_ai_service_message_st;
   uintptr_t timer_tag = (uintptr_t)&state->timer;

   gfx_animation_kill_by_tag(&timer_tag);
   state->visible = false;

   if (cancel_pending)
   {
      state->message_updated = false;
      state->message[0]      = '\0';
      state->wrapped[0]      = '\0';
      state->message_len     = 0;
      state->wrapped_len     = 0;
      state->target_language = TRANSLATION_LANG_DONT_CARE;
   }
}

static void gfx_widget_ai_service_message_expired_cb(void *userdata)
{
   (void)userdata;
   p_w_ai_service_message_st.visible = false;
}

static gfx_widget_font_data_t *gfx_widget_ai_service_message_font(
      dispgfx_widget_t *p_dispwidget)
{
   gfx_widget_ai_service_message_state_t *state =
         &p_w_ai_service_message_st;

   if (   state->target_language == TRANSLATION_LANG_KO
       && p_dispwidget->gfx_widget_fonts.ai_service_korean.font)
      return &p_dispwidget->gfx_widget_fonts.ai_service_korean;

   return &p_dispwidget->gfx_widget_fonts.msg_queue;
}

static size_t gfx_widget_ai_service_utf8_sequence_len(
      const unsigned char *text, size_t remaining)
{
   unsigned char lead;

   if (!text || remaining == 0)
      return 0;

   lead = text[0];
   if (lead < 0x80)
      return 1;
   if (lead >= 0xC2 && lead <= 0xDF)
      return remaining >= 2 && (text[1] & 0xC0) == 0x80 ? 2 : 0;
   if (lead >= 0xE0 && lead <= 0xEF)
   {
      if (remaining < 3 || (text[2] & 0xC0) != 0x80)
         return 0;
      if (lead == 0xE0)
         return text[1] >= 0xA0 && text[1] <= 0xBF ? 3 : 0;
      if (lead == 0xED)
         return text[1] >= 0x80 && text[1] <= 0x9F ? 3 : 0;
      return (text[1] & 0xC0) == 0x80 ? 3 : 0;
   }
   if (lead >= 0xF0 && lead <= 0xF4)
   {
      if (remaining < 4
            || (text[2] & 0xC0) != 0x80
            || (text[3] & 0xC0) != 0x80)
         return 0;
      if (lead == 0xF0)
         return text[1] >= 0x90 && text[1] <= 0xBF ? 4 : 0;
      if (lead == 0xF4)
         return text[1] >= 0x80 && text[1] <= 0x8F ? 4 : 0;
      return (text[1] & 0xC0) == 0x80 ? 4 : 0;
   }
   return 0;
}

static void gfx_widget_ai_service_sanitize_utf8(
      char *text, size_t text_len)
{
   size_t offset = 0;

   while (offset < text_len)
   {
      size_t sequence_len = gfx_widget_ai_service_utf8_sequence_len(
            (const unsigned char*)text + offset, text_len - offset);
      if (sequence_len == 0)
      {
         text[offset++] = '?';
         continue;
      }
      offset += sequence_len;
   }
}

static void gfx_widget_ai_service_truncate_wrapped(
      gfx_widget_ai_service_message_state_t *state, char *at)
{
   size_t used = (size_t)(at - state->wrapped);

   *at = '\0';
   if (used + 4 <= sizeof(state->wrapped))
   {
      memcpy(state->wrapped + used, "...", 4);
      used += 3;
   }
   state->wrapped_len = used;
}

/* word_wrap_wideglyph() preserves long ASCII tokens without spaces. Enforce
 * a hard visible-width limit as a second pass and stop after max_lines, so a
 * malformed model response cannot make the renderer process thousands of
 * clipped glyphs on every frame. */
static unsigned gfx_widget_ai_service_hard_wrap(
      gfx_widget_ai_service_message_state_t *state,
      unsigned line_width, unsigned max_lines)
{
   char *cursor       = state->wrapped;
   char *end          = state->wrapped + state->wrapped_len;
   unsigned lines     = 1;
   unsigned line_used = 0;

   while (cursor < end && *cursor)
   {
      size_t remaining;
      size_t sequence_len;
      unsigned glyph_width;

      if (*cursor == '\n')
      {
         if (lines >= max_lines)
         {
            gfx_widget_ai_service_truncate_wrapped(state, cursor);
            return lines;
         }
         lines++;
         line_used = 0;
         cursor++;
         continue;
      }

      remaining = (size_t)(end - cursor);
      sequence_len = gfx_widget_ai_service_utf8_sequence_len(
            (const unsigned char*)cursor, remaining);
      if (sequence_len == 0)
         sequence_len = 1;
      glyph_width = sequence_len >= 3 ? 2 : 1;

      if (line_used > 0 && line_used + glyph_width > line_width)
      {
         size_t offset = (size_t)(cursor - state->wrapped);

         if (lines >= max_lines
               || state->wrapped_len + 2 > sizeof(state->wrapped))
         {
            gfx_widget_ai_service_truncate_wrapped(state, cursor);
            return lines;
         }

         memmove(cursor + 1, cursor,
               state->wrapped_len - offset + 1);
         *cursor = '\n';
         state->wrapped_len++;
         end++;
         lines++;
         line_used = 0;
         cursor++;
         continue;
      }

      line_used += glyph_width;
      cursor    += sequence_len;
   }

   return lines;
}

static void gfx_widget_ai_service_message_update_layout(
      dispgfx_widget_t *p_dispwidget)
{
   gfx_widget_ai_service_message_state_t *state =
         &p_w_ai_service_message_st;
   gfx_widget_font_data_t *font_msg_queue =
         gfx_widget_ai_service_message_font(p_dispwidget);
   unsigned video_width  = p_dispwidget->last_video_width;
   unsigned video_height = p_dispwidget->last_video_height;
   unsigned max_bg_width;
   unsigned max_text_width;
   unsigned max_lines;
   unsigned line_count;
   unsigned max_line_width = 0;
   int line_width;
   char *line;

   if (!state->message[0] || video_width == 0 || video_height == 0
         || !font_msg_queue->font || font_msg_queue->line_height <= 0.0f)
      return;

   state->text_padding = (unsigned)(
         (font_msg_queue->line_height * (2.0f / 3.0f)) + 0.5f);
   state->frame_width  = p_dispwidget->divider_width_1px;
   state->text_color   = TEXT_COLOR_INFO;

   max_bg_width = (unsigned)((float)video_width * 0.92f);
   if (max_bg_width > video_width)
      max_bg_width = video_width;
   if (max_bg_width <= state->text_padding * 2)
      max_bg_width = video_width;
   max_text_width = max_bg_width > state->text_padding * 2
         ? max_bg_width - state->text_padding * 2 : 1;

   line_width = font_msg_queue->glyph_width > 0.0f
         ? (int)((float)max_text_width / font_msg_queue->glyph_width)
         : 40;
   if (line_width < 1)
      line_width = 1;

   state->wrapped_len = word_wrap_wideglyph(
         state->wrapped, sizeof(state->wrapped),
         state->message, state->message_len,
         line_width, 200, 0);
   if (state->wrapped_len == 0 && state->message_len > 0)
      state->wrapped_len = gfx_widget_copy_message(
            state->wrapped, sizeof(state->wrapped), state->message);

   max_lines = (unsigned)(((float)video_height * 0.42f)
         / font_msg_queue->line_height);
   if (max_lines < 1)
      max_lines = 1;
   if (max_lines > AI_SERVICE_MESSAGE_MAX_LINES)
      max_lines = AI_SERVICE_MESSAGE_MAX_LINES;

   line_count = gfx_widget_ai_service_hard_wrap(
         state, (unsigned)line_width, max_lines);
   line       = state->wrapped;
   while (line && *line)
   {
      char *newline = strchr(line, '\n');
      size_t line_len = newline
            ? (size_t)(newline - line) : strlen(line);
      int measured_width = font_driver_get_message_width(
            font_msg_queue->font, line, line_len, 1.0f);

      if (measured_width > 0
            && (unsigned)measured_width > max_line_width)
         max_line_width = (unsigned)measured_width;

      if (!newline)
         break;
      line = newline + 1;
   }

   if (max_line_width > max_text_width)
      max_line_width = max_text_width;
   state->line_count = line_count;
   state->bg_width   = max_line_width + state->text_padding * 2;
   if (state->bg_width > max_bg_width)
      state->bg_width = max_bg_width;
   state->bg_height  = (unsigned)(
         (font_msg_queue->line_height * state->line_count)
         + state->text_padding * 2);

   state->bg_x   = ((float)video_width - (float)state->bg_width) * 0.5f;
   state->bg_y   = (float)video_height - (float)state->bg_height
         - (float)p_w_generic_message_st.bg_height
         - (float)state->text_padding;
   if (state->bg_y < 0.0f)
      state->bg_y = 0.0f;
   state->text_x = state->bg_x + (float)state->text_padding;
   state->text_y = state->bg_y + (float)state->text_padding
         + (font_msg_queue->line_height * 0.5f)
         + font_msg_queue->line_centre_offset;
}
#endif

/* Callbacks */

static void gfx_widget_generic_message_fade_out_cb(void *userdata)
{
   gfx_widget_generic_message_reset(false);
}

static void gfx_widget_generic_message_wait_cb(void *userdata)
{
   gfx_widget_generic_message_state_t *state = (gfx_widget_generic_message_state_t*)userdata;
   uintptr_t alpha_tag                       = (uintptr_t)&state->alpha;
   gfx_animation_ctx_entry_t animation_entry;

   /* Trigger fade out */
   state->alpha                 = 1.0f;
   animation_entry.easing_enum  = EASING_OUT_QUAD;
   animation_entry.tag          = alpha_tag;
   animation_entry.duration     = GENERIC_MESSAGE_FADE_DURATION;
   animation_entry.target_value = 0.0f;
   animation_entry.subject      = &state->alpha;
   animation_entry.cb           = gfx_widget_generic_message_fade_out_cb;
   animation_entry.userdata     = NULL;

   gfx_animation_push(&animation_entry);
   state->status = GFX_WIDGET_GENERIC_MESSAGE_FADE_OUT;
}

static void gfx_widget_generic_message_slide_in_cb(void *userdata)
{
   gfx_widget_generic_message_state_t *state = (gfx_widget_generic_message_state_t*)userdata;
   gfx_timer_ctx_entry_t timer;

   /* Start wait timer */
   state->alpha   = 1.0f;
   timer.duration = state->message_duration;
   timer.cb       = gfx_widget_generic_message_wait_cb;
   timer.userdata = state;

   gfx_animation_timer_start(&state->timer, &timer);
   state->status = GFX_WIDGET_GENERIC_MESSAGE_WAIT;
}

/* Widget interface */
void gfx_widget_set_generic_message(
      const char *msg, unsigned duration)
{
   dispgfx_widget_t *p_dispwidget            = dispwidget_get_ptr();
   gfx_widget_generic_message_state_t *state = &p_w_generic_message_st;
   unsigned last_video_width                 = p_dispwidget->last_video_width;
   int text_width                            = 0;
   gfx_widget_font_data_t *font_msg_queue    = &p_dispwidget->gfx_widget_fonts.msg_queue;

   /* Ensure we have a valid message string */
   if (!msg || !*msg)
      return;

   /* Cache message parameters */
   state->message_len      = gfx_widget_copy_message(
         state->message, sizeof(state->message), msg);
   state->message_duration = duration;

   /* Get background width */
   text_width         = font_driver_get_message_width(
         font_msg_queue->font, state->message,
         state->message_len, 1.0f);
   if (text_width < 0)
      text_width      = 0;
   state->bg_width    = (state->text_padding * 2) + (unsigned)text_width;

   if (state->bg_width < state->bg_min_width)
      state->bg_width = state->bg_min_width;
   if (state->bg_width > last_video_width)
      state->bg_width = last_video_width;

   /* Update x draw positions */
   state->bg_x        = ((float)last_video_width 
         - (float)state->bg_width) * 0.5f;
   state->text_x      = ((float)last_video_width 
         - (float)text_width) * 0.5f;
   if (state->text_x < (float)state->text_padding)
      state->text_x   = (float)state->text_padding;

   /* If a 'slide in' animation is already in
    * progress, no further action is required;
    * just let animation continue with the updated
    * message text */
   if (state->status == GFX_WIDGET_GENERIC_MESSAGE_SLIDE_IN)
      return;

   /* Signal that message has been updated
    * > Note that we have to defer the triggering
    *   of any animation changes until the next
    *   call of gfx_widget_generic_message_iterate().
    *   This is because cores will often send messages
    *   during initialisation - i.e. during processes
    *   that take a non-trivial amount of time. In these
    *   cases, updating the animation state here would
    *   result in the following:
    *   - Core starts initialisation/load content
    *   - Message is set, animation is triggered
    *   - Core finishes initialisation/load content,
    *     taking multiple 100's of ms
    *   - On next runloop iterate, animation status
    *     is checked - but because initialisation
    *     took so long, the animation duration has
    *     already elapsed
    *   - Animation 'finishes' immediately, and the
    *     user never sees it... */
   state->message_updated = true;
}

#ifdef HAVE_TRANSLATE
void gfx_widget_set_ai_service_message(
      const char *msg, unsigned duration,
      unsigned target_language)
{
   gfx_widget_ai_service_message_state_t *state =
         &p_w_ai_service_message_st;

   if (!msg || !*msg)
      return;

   state->message_len = gfx_widget_copy_message(
         state->message, sizeof(state->message), msg);
   if (state->message_len == 0)
      return;

   gfx_widget_ai_service_sanitize_utf8(
         state->message, state->message_len);

   state->message_duration = duration;
   state->target_language  = target_language;
   state->message_updated  = true;
}

void gfx_widget_clear_ai_service_message(void)
{
   gfx_widget_ai_service_message_reset(true);
}
#endif

/* Widget layout() */

static void gfx_widget_generic_message_layout(
      void *data,
      bool is_threaded, const char *dir_assets, char *font_path)
{
   dispgfx_widget_t *p_dispwidget            = (dispgfx_widget_t*)data;
   gfx_widget_generic_message_state_t *state = &p_w_generic_message_st;

   unsigned last_video_width                 = p_dispwidget->last_video_width;
   unsigned last_video_height                = p_dispwidget->last_video_height;
   unsigned divider_width                    = p_dispwidget->divider_width_1px;
   unsigned widget_padding                   = 0;
   int text_width                            = 0;
   const float base_aspect                   = 4.0f / 3.0f;
   float widget_margin                       = 0.0f;
   gfx_widget_font_data_t *font_msg_queue    = &p_dispwidget->gfx_widget_fonts.msg_queue;

   /* Set padding values */
   state->text_padding = (unsigned)(((float)font_msg_queue->line_height * (2.0f / 3.0f)) + 0.5f);
   widget_padding      = state->text_padding * 2;

   /* Set frame width */
   state->frame_width  = divider_width * 2;

   /* Set minimum background width
    * > Widget nominally occupies the full width
    *   of a 4:3 region at the centre of the screen,
    *   minus padding (it may expand if the text is
    *   too long to fit within these bounds) */
   widget_margin          = (float)last_video_width -
         (base_aspect * (float)last_video_height);
   if (widget_margin < 0.0f)
      widget_margin       = 0.0f;

   state->bg_min_width    = last_video_width - (unsigned)widget_margin -
         ((widget_padding + state->frame_width) * 2);
   if (state->bg_min_width > last_video_width)
      state->bg_min_width = last_video_width;

   /* Set background width */
   state->bg_width     = state->text_padding * 2;

   if (*state->message)
   {
      text_width       = font_driver_get_message_width(
            font_msg_queue->font, state->message,
            state->message_len, 1.0f);
      if (text_width < 0)
         text_width       = 0;

      state->bg_width += (unsigned)text_width;
   }

   if (state->bg_width < state->bg_min_width)
      state->bg_width     = state->bg_min_width;
   if (state->bg_width > last_video_width)
      state->bg_width     = last_video_width;

   /* Set background height */
   state->bg_height    = font_msg_queue->line_height * 2;

   /* Set background draw positions */
   state->bg_x         = ((float)last_video_width - (float)state->bg_width) * 0.5f;
   state->bg_y_start   = (float)last_video_height + (float)state->frame_width;
   state->bg_y_end     = (float)last_video_height - (float)state->bg_height;

   /* Set text draw positions
    * > Text is drawn at the horizontal centre
    *   of the screen - unless it is too long to
    *   fit within the bounds of the window, in
    *   which case we shift it to the right such
    *   that the start of the string can be seen */
   state->text_x          = ((float)last_video_width 
         - (float)text_width) * 0.5f;
   if (state->text_x < (float)state->text_padding)
      state->text_x       = (float)state->text_padding;
   state->text_y_start    = state->bg_y_start + 
      ((float)state->bg_height * 0.5f) +
      (float)font_msg_queue->line_centre_offset;
   state->text_y_end      = state->bg_y_end + 
      ((float)state->bg_height * 0.5f) +
      (float)font_msg_queue->line_centre_offset;

#ifdef HAVE_TRANSLATE
   if (p_w_ai_service_message_st.message[0])
      gfx_widget_ai_service_message_update_layout(p_dispwidget);
#endif
}

/* Widget iterate() */

static void gfx_widget_generic_message_iterate(void *user_data,
      unsigned width, unsigned height, bool fullscreen,
      const char *dir_assets, char *font_path,
      bool is_threaded)
{
   dispgfx_widget_t *p_dispwidget = (dispgfx_widget_t*)user_data;
   gfx_widget_generic_message_state_t *state = &p_w_generic_message_st;

#ifdef HAVE_TRANSLATE
   gfx_widget_ai_service_message_state_t *ai_state =
         &p_w_ai_service_message_st;

   if (ai_state->message_updated)
   {
      gfx_timer_ctx_entry_t timer;
      uintptr_t timer_tag = (uintptr_t)&ai_state->timer;

      gfx_animation_kill_by_tag(&timer_tag);
      gfx_widget_ai_service_message_update_layout(p_dispwidget);

      /* duration == 0 is an explicit persistent subtitle. It remains visible
       * until the next translation replaces it or a lifecycle/stop path calls
       * gfx_widget_clear_ai_service_message(). */
      if (ai_state->message_duration > 0)
      {
         timer.duration = ai_state->message_duration;
         timer.cb       = gfx_widget_ai_service_message_expired_cb;
         timer.userdata = ai_state;
         gfx_animation_timer_start(&ai_state->timer, &timer);
      }

      ai_state->visible         = true;
      ai_state->message_updated = false;
   }
#else
   (void)p_dispwidget;
#endif

   if (state->message_updated)
   {
      enum gfx_widget_generic_message_status current_status = state->status;
      uintptr_t alpha_tag                                   = (uintptr_t)&state->alpha;
      gfx_animation_ctx_entry_t animation_entry;

      /* In all cases, reset any existing animation */
      gfx_widget_generic_message_reset(false);

      /* If an animation was already in progress,
       * have to continue from the last active
       * animation phase */
      switch (current_status)
      {
         case GFX_WIDGET_GENERIC_MESSAGE_IDLE:
            /* Trigger 'slide in' animation */
            state->alpha                 = 0.0f;
            animation_entry.easing_enum  = EASING_OUT_QUAD;
            animation_entry.tag          = alpha_tag;
            animation_entry.duration     = GENERIC_MESSAGE_FADE_DURATION;
            animation_entry.target_value = 1.0f;
            animation_entry.subject      = &state->alpha;
            animation_entry.cb           = gfx_widget_generic_message_slide_in_cb;
            animation_entry.userdata     = state;

            gfx_animation_push(&animation_entry);
            state->status = GFX_WIDGET_GENERIC_MESSAGE_SLIDE_IN;
            break;
         case GFX_WIDGET_GENERIC_MESSAGE_FADE_IN:
         case GFX_WIDGET_GENERIC_MESSAGE_FADE_OUT:
            {
               /* If we are fading in or out, start
                * a new fade in animation (transitioning
                * from the current alpha value to 1.0) */
               unsigned fade_duration = (unsigned)(((1.0f - state->alpha) *
                     (float)GENERIC_MESSAGE_FADE_DURATION) + 0.5f);
               if (fade_duration > GENERIC_MESSAGE_FADE_DURATION)
                  fade_duration = GENERIC_MESSAGE_FADE_DURATION;

               /* > If current and final alpha values are the
                *   same, or fade duration is zero, skip
                *   straight to the wait phase */
               if ((state->alpha >= 1.0f) || (fade_duration < 1))
                  gfx_widget_generic_message_slide_in_cb(state);
               else
               {
                  animation_entry.easing_enum  = EASING_OUT_QUAD;
                  animation_entry.tag          = alpha_tag;
                  animation_entry.duration     = fade_duration;
                  animation_entry.target_value = 1.0f;
                  animation_entry.subject      = &state->alpha;
                  /* Note that 'slide in' and 'fade in' share
                   * the same callback */
                  animation_entry.cb           = gfx_widget_generic_message_slide_in_cb;
                  animation_entry.userdata     = state;

                  gfx_animation_push(&animation_entry);
                  state->status = GFX_WIDGET_GENERIC_MESSAGE_FADE_IN;
               }
            }
            break;
         case GFX_WIDGET_GENERIC_MESSAGE_WAIT:
            /* If we are currently waiting, just
             * 'reset' the wait timer */
            gfx_widget_generic_message_slide_in_cb(state);
            break;
         default:
            /* The only remaining case is
             * GFX_WIDGET_GENERIC_MESSAGE_SLIDE_IN,
             * which can never happen (state->message_updated
             * will be false if it is, so this code will
             * not be invoked). If we reach this point, an
             * unknown error has occurred. We have already
             * reset any existing animation, so no further
             * action is required */
            break;
      }

      state->message_updated = false;
   }
}

/* Widget frame() */

#ifdef HAVE_TRANSLATE
static void gfx_widget_ai_service_message_frame(
      video_frame_info_t *video_info,
      dispgfx_widget_t *p_dispwidget)
{
   gfx_widget_ai_service_message_state_t *state =
         &p_w_ai_service_message_st;
   unsigned video_width;
   unsigned video_height;
   void *userdata;
   gfx_display_t *p_disp;
   gfx_display_ctx_driver_t *dispctx;
   gfx_widget_font_data_t *font_msg_queue;
   unsigned text_color;

   if (!state->visible || !state->wrapped[0])
      return;

   video_width    = video_info->width;
   video_height   = video_info->height;
   userdata       = video_info->userdata;
   p_disp         = (gfx_display_t*)video_info->disp_userdata;
   dispctx        = p_disp ? p_disp->dispctx : NULL;
   font_msg_queue = gfx_widget_ai_service_message_font(p_dispwidget);
   text_color     = COLOR_TEXT_ALPHA(state->text_color, 255);

   gfx_display_set_alpha(p_w_generic_message_st.bg_color, 0.88f);
   gfx_display_set_alpha(p_w_generic_message_st.frame_color, 0.92f);

   gfx_display_draw_quad(
         p_disp, userdata, video_width, video_height,
         state->bg_x, state->bg_y,
         state->bg_width, state->bg_height,
         video_width, video_height,
         p_w_generic_message_st.bg_color, NULL);

   gfx_display_draw_quad(
         p_disp, userdata, video_width, video_height,
         state->bg_x - (float)state->frame_width,
         state->bg_y - (float)state->frame_width,
         state->bg_width + state->frame_width * 2,
         state->frame_width,
         video_width, video_height,
         p_w_generic_message_st.frame_color, NULL);
   gfx_display_draw_quad(
         p_disp, userdata, video_width, video_height,
         state->bg_x - (float)state->frame_width,
         state->bg_y,
         state->frame_width, state->bg_height,
         video_width, video_height,
         p_w_generic_message_st.frame_color, NULL);
   gfx_display_draw_quad(
         p_disp, userdata, video_width, video_height,
         state->bg_x + (float)state->bg_width,
         state->bg_y,
         state->frame_width, state->bg_height,
         video_width, video_height,
         p_w_generic_message_st.frame_color, NULL);
   gfx_display_draw_quad(
         p_disp, userdata, video_width, video_height,
         state->bg_x - (float)state->frame_width,
         state->bg_y + (float)state->bg_height,
         state->bg_width + state->frame_width * 2,
         state->frame_width,
         video_width, video_height,
         p_w_generic_message_st.frame_color, NULL);

   /* Flush glyphs batched by widgets drawn earlier in the frame before
    * enabling this overlay's clip rectangle. Otherwise unrelated text (for
    * example leaderboard widgets) would be clipped to the translation box. */
   if (font_msg_queue != &p_dispwidget->gfx_widget_fonts.msg_queue)
      gfx_widgets_flush_text(video_width, video_height,
            &p_dispwidget->gfx_widget_fonts.msg_queue);
   gfx_widgets_flush_text(video_width, video_height, font_msg_queue);
   gfx_display_scissor_begin(
         p_disp, userdata, video_width, video_height,
         (int)state->bg_x, (int)state->bg_y,
         state->bg_width, state->bg_height);
   gfx_widgets_draw_text(
         font_msg_queue, state->wrapped,
         state->text_x, state->text_y,
         video_width, video_height,
         text_color, TEXT_ALIGN_LEFT, true);
   gfx_widgets_flush_text(video_width, video_height, font_msg_queue);
   if (dispctx && dispctx->scissor_end)
      dispctx->scissor_end(userdata, video_width, video_height);
}
#endif

static void gfx_widget_generic_message_frame(void *data, void *user_data)
{
   gfx_widget_generic_message_state_t *state = &p_w_generic_message_st;

#ifdef HAVE_TRANSLATE
   gfx_widget_ai_service_message_frame(
         (video_frame_info_t*)data, (dispgfx_widget_t*)user_data);
#endif

   if (state->status != GFX_WIDGET_GENERIC_MESSAGE_IDLE)
   {
      unsigned text_color;
      float widget_alpha, bg_y, text_y;
      video_frame_info_t *video_info         = (video_frame_info_t*)data;
      dispgfx_widget_t *p_dispwidget         = (dispgfx_widget_t*)user_data;

      unsigned video_width                   = video_info->width;
      unsigned video_height                  = video_info->height;
      void *userdata                         = video_info->userdata;
      gfx_display_t *p_disp                  = (gfx_display_t*)video_info->disp_userdata;
      gfx_widget_font_data_t *font_msg_queue = &p_dispwidget->gfx_widget_fonts.msg_queue;
      size_t msg_queue_size                  = p_dispwidget->current_msgs_size;

      /* Determine status-dependent opacity/position
       * values */
      switch (state->status)
      {
         case GFX_WIDGET_GENERIC_MESSAGE_SLIDE_IN:
            widget_alpha = state->alpha;
            /* Use 'alpha' to determine the draw offset
             * > Saves having to trigger two animations */
            bg_y         = state->bg_y_start + (state->alpha *
                  (state->bg_y_end - state->bg_y_start));
            text_y       = state->text_y_start + (state->alpha *
                  (state->text_y_end - state->text_y_start));
            break;
         case GFX_WIDGET_GENERIC_MESSAGE_FADE_IN:
         case GFX_WIDGET_GENERIC_MESSAGE_FADE_OUT:
            widget_alpha = state->alpha;
            bg_y         = state->bg_y_end;
            text_y       = state->text_y_end;
            break;
         case GFX_WIDGET_GENERIC_MESSAGE_WAIT:
            widget_alpha = 1.0f;
            bg_y         = state->bg_y_end;
            text_y       = state->text_y_end;
            break;
         default:
            widget_alpha = 0.0f;
            bg_y         = state->bg_y_start;
            text_y       = state->text_y_start;
            break;
      }

      /* Draw widget */
      if (widget_alpha > 0.0f)
      {
         /* Set opacity */
         gfx_display_set_alpha(state->bg_color, widget_alpha);
         gfx_display_set_alpha(state->frame_color, widget_alpha);
         text_color = COLOR_TEXT_ALPHA(state->text_color,
               (unsigned)(widget_alpha * 255.0f));

         /* Background */
         gfx_display_draw_quad(
               p_disp,
               userdata,
               video_width,
               video_height,
               state->bg_x,
               bg_y,
               state->bg_width,
               state->bg_height,
               video_width,
               video_height,
               state->bg_color,
               NULL);

         /* Frame */

         /* > Top */
         gfx_display_draw_quad(
               p_disp,
               userdata,
               video_width,
               video_height,
               state->bg_x - (float)state->frame_width,
               bg_y - (float)state->frame_width,
               state->bg_width + (state->frame_width * 2),
               state->frame_width,
               video_width,
               video_height,
               state->frame_color,
               NULL);

         /* > Left */
         gfx_display_draw_quad(
               p_disp,
               userdata,
               video_width,
               video_height,
               state->bg_x - (float)state->frame_width,
               bg_y,
               state->frame_width,
               state->bg_height,
               video_width,
               video_height,
               state->frame_color,
               NULL);

         /* > Right */
         gfx_display_draw_quad(
               p_disp,
               userdata,
               video_width,
               video_height,
               state->bg_x + (float)state->bg_width,
               bg_y,
               state->frame_width,
               state->bg_height,
               video_width,
               video_height,
               state->frame_color,
               NULL);

         /* Message */
         gfx_widgets_draw_text(
               font_msg_queue,
               state->message,
               state->text_x,
               text_y,
               video_width,
               video_height,
               text_color,
               TEXT_ALIGN_LEFT,
               true);

         /* If the message queue is active, must flush the
          * text here to avoid overlaps */
         if (msg_queue_size > 0)
            gfx_widgets_flush_text(video_width, video_height,
                  font_msg_queue);
      }
   }
}

/* Widget free() */

static void gfx_widget_generic_message_free(void)
{
   gfx_widget_generic_message_reset(true);
#ifdef HAVE_TRANSLATE
   gfx_widget_ai_service_message_reset(true);
#endif
}

/* Widget definition */

static bool gfx_widget_generic_message_visible(void)
{
   gfx_widget_generic_message_state_t *state = &p_w_generic_message_st;
#ifdef HAVE_TRANSLATE
   if (p_w_ai_service_message_st.visible)
      return true;
#endif
   return state->status != GFX_WIDGET_GENERIC_MESSAGE_IDLE;
}

const gfx_widget_t gfx_widget_generic_message = {
   NULL, /* init */
   gfx_widget_generic_message_free,
   NULL, /* context_reset*/
   NULL, /* context_destroy */
   gfx_widget_generic_message_layout,
   gfx_widget_generic_message_iterate,
   gfx_widget_generic_message_frame,
   gfx_widget_generic_message_visible
};
