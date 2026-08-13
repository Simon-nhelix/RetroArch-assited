/* Single-source definitions: AI service options group.
 * Grammar identical to settings_def_video_sync.h plus S_FLOAT and
 * the _NS no-sublabel variants; the descriptor argument span
 * matches SDESC_<kind>_ROW; row order is menu display order;
 * h2json.py parses these rows for the Crowdin source upload. */

/* Descriptor and configuration rows are #ifdef HAVE_TRANSLATE; the string
 * tables always carry this row via the strings pass. */
#if defined(HAVE_TRANSLATE) || defined(SETTINGS_DEF_STRINGS_PASS)
S_BOOL_EX(ai_service_enable, AI_SERVICE_ENABLE,
      "ai_service_enable",
      DEFAULT_AI_SERVICE_ENABLE, SD_FLAG_NONE, 0, 0, setting_bool_action_left_with_refresh, NULL, NULL, NULL, setting_bool_action_left_with_refresh, setting_bool_action_right_with_refresh, 0,
      "AI Service Enabled",
      "Enable AI Service to run when the AI Service hotkey is pressed.")
#endif
/* Descriptor and configuration rows are #ifdef HAVE_TRANSLATE; the string
 * tables always carry this row via the strings pass. */
#if defined(HAVE_TRANSLATE) || defined(SETTINGS_DEF_STRINGS_PASS)
S_BOOL(ai_service_pause, AI_SERVICE_PAUSE,
      "ai_service_pause",
      DEFAULT_AI_SERVICE_PAUSE, SD_FLAG_NONE, 0, 0,
      "Pause During Translation",
      "Pause core while screen is translated.")
#endif
/* Descriptor and configuration rows are #ifdef HAVE_TRANSLATE; the string
 * tables always carry this row via the strings pass. */
#if defined(HAVE_TRANSLATE) || defined(SETTINGS_DEF_STRINGS_PASS)
S_UINT_EX(ai_service_timeout, AI_SERVICE_TIMEOUT,
      "ai_service_timeout",
      DEFAULT_AI_SERVICE_TIMEOUT, SD_FLAG_NONE, SDESC_RANGE_MINMAX, 0, 0, 300, 1, 0, setting_action_ok_uint, NULL, NULL, NULL, NULL, NULL, 0,
      "Translation Request Timeout",
      "Seconds to wait for a translation HTTP response before cancelling and retrying. 0 disables the timeout.")
#endif
/* Descriptor and configuration rows are #ifdef HAVE_TRANSLATE; the string
 * tables always carry this row via the strings pass. */
#if defined(HAVE_TRANSLATE) || defined(SETTINGS_DEF_STRINGS_PASS)
S_UINT_EX(ai_service_source_lang, AI_SERVICE_SOURCE_LANG,
      "ai_service_source_lang",
      DEFAULT_AI_SERVICE_SOURCE_LANG, SD_FLAG_NONE, SDESC_RANGE_MINMAX, 0, TRANSLATION_LANG_DONT_CARE, (TRANSLATION_LANG_LAST-1), 1, 0, setting_action_ok_uint, setting_get_string_representation_uint_ai_service_lang, NULL, NULL, NULL, NULL, 0,
      "Source Language",
      "The language the service will translate from. If set to 'Default', it will attempt to auto-detect the language. Setting it to a specific language will make the translation more accurate.")
#endif
/* Descriptor and configuration rows are #ifdef HAVE_TRANSLATE; the string
 * tables always carry this row via the strings pass. */
#if defined(HAVE_TRANSLATE) || defined(SETTINGS_DEF_STRINGS_PASS)
S_UINT_EX(ai_service_target_lang, AI_SERVICE_TARGET_LANG,
      "ai_service_target_lang",
      DEFAULT_AI_SERVICE_TARGET_LANG, SD_FLAG_NONE, SDESC_RANGE_MINMAX, 0, TRANSLATION_LANG_DONT_CARE, (TRANSLATION_LANG_LAST-1), 1, 0, setting_action_ok_uint, setting_get_string_representation_uint_ai_service_lang, NULL, NULL, NULL, NULL, 0,
      "Output Language",
      "The language shown or spoken by the translation service. 'Default' is English.")
#endif
