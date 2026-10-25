// sylpheed - ReXGlue Recompiled Project
//
// SYLPHEED_* logging macros bound to the sylpheed log category. Ported from reblue's
// core/logging.h (Copyright (c) 2026 Tom Clay, BSD 3-Clause).

#pragma once

#include <atomic>

#include <rex/logging.h>

#include "core/settings.h"

REXLOG_DEFINE_CATEGORY(sylpheed)

#define SYLPHEED_TRACE(...) REXLOG_CAT_TRACE(::rex::log::sylpheed(), __VA_ARGS__)
#define SYLPHEED_DEBUG(...) REXLOG_CAT_DEBUG(::rex::log::sylpheed(), __VA_ARGS__)
#define SYLPHEED_INFO(...) REXLOG_CAT_INFO(::rex::log::sylpheed(), __VA_ARGS__)
#define SYLPHEED_WARN(...) REXLOG_CAT_WARN(::rex::log::sylpheed(), __VA_ARGS__)
#define SYLPHEED_ERROR(...) REXLOG_CAT_ERROR(::rex::log::sylpheed(), __VA_ARGS__)
#define SYLPHEED_CRITICAL(...) REXLOG_CAT_CRITICAL(::rex::log::sylpheed(), __VA_ARGS__)

#define SYLPHEED_DEV_WARN(...)                                                      \
  do {                                                                         \
    if (::sylpheed::Settings::Get().Devmode())                                      \
      SYLPHEED_WARN(__VA_ARGS__);                                                   \
  } while (0)

#define SYLPHEED_DEV_INFO(...)                                                      \
  do {                                                                         \
    if (::sylpheed::Settings::Get().Devmode())                                      \
      SYLPHEED_INFO(__VA_ARGS__);                                                   \
  } while (0)

// Warns on the first `limit` hits of one call site. Marks paths the renderer
// doesn't model yet, so a run shows which ones the game reaches.
#define SYLPHEED_WARN_FIRST(limit, ...)                                             \
  do {                                                                         \
    static std::atomic<unsigned> s_sylpheed_warned{0};                              \
    if (s_sylpheed_warned.fetch_add(1, std::memory_order_relaxed) < (limit))        \
      SYLPHEED_WARN(__VA_ARGS__);                                                   \
  } while (0)

// The same at the debug level, for paths that are modelled but worth seeing
// while debugging.
#define SYLPHEED_DEBUG_FIRST(limit, ...)                                            \
  do {                                                                         \
    static std::atomic<unsigned> s_sylpheed_noted{0};                               \
    if (s_sylpheed_noted.fetch_add(1, std::memory_order_relaxed) < (limit))        \
      SYLPHEED_DEBUG(__VA_ARGS__);                                                  \
  } while (0)
