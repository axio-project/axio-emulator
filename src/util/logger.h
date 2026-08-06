#pragma once

/***************************************************************************
 *   Copyright (C) 2008 by H-Store Project                                 *
 *   Brown University                                                      *
 *   Massachusetts Institute of Technology                                 *
 *   Yale University                                                       *
 *                                                                         *
 *   This software may be modified and distributed under the terms         *
 *   of the MIT license.  See the LICENSE file for details.                *
 *                                                                         *
 ***************************************************************************/

/**
 * @file logger.h
 * @brief Logging macros that can be optimized out
 * @author Hideaki and Anuj, modified by Xinyang
 */

#include <chrono>
#include <string>

namespace axio {

// Log levels: higher means more verbose
#define AXIO_LOG_LEVEL_OFF 0
#define AXIO_LOG_LEVEL_ERROR 1  // Only fatal conditions
#define AXIO_LOG_LEVEL_WARN 2  // Conditions from which it's possible to recover
#define AXIO_LOG_LEVEL_INFO 3  // Reasonable to log (e.g., management packets)
#define AXIO_LOG_LEVEL_REORDER 4  // Too frequent to log (e.g., reordered pkts)
#define AXIO_LOG_LEVEL_TRACE 5  // Extremely frequent (e.g., all datapath pkts)
#define AXIO_LOG_LEVEL_CC 6     // Even congestion control decisions!

#define AXIO_LOG_DEFAULT_STREAM stdout

// Log messages with "reorder" or higher verbosity get written to
// axio_trace_file_or_default_stream. This can be stdout for basic debugging, or
// Axio's trace file for more involved debugging.

#define axio_trace_file_or_default_stream trace_file_
//#define axio_trace_file_or_default_stream AXIO_LOG_DEFAULT_STREAM

// If AXIO_LOG_LEVEL is not defined, default to the highest level so that
// YouCompleteMe does not report compilation errors
#ifndef AXIO_LOG_LEVEL
#define AXIO_LOG_LEVEL AXIO_LOG_LEVEL_CC
#endif

#if AXIO_LOG_LEVEL >= AXIO_LOG_LEVEL_ERROR
#define AXIO_ERROR(...)                                 \
  axio::output_log_header(stderr, AXIO_LOG_LEVEL_ERROR); \
  fprintf(AXIO_LOG_DEFAULT_STREAM, __VA_ARGS__);        \
  fflush(AXIO_LOG_DEFAULT_STREAM)
#else
#define AXIO_ERROR(...) ((void)0)
#endif

#if AXIO_LOG_LEVEL >= AXIO_LOG_LEVEL_WARN
#define AXIO_WARN(...)                                                  \
  axio::output_log_header(AXIO_LOG_DEFAULT_STREAM, AXIO_LOG_LEVEL_WARN); \
  fprintf(AXIO_LOG_DEFAULT_STREAM, __VA_ARGS__);                        \
  fflush(AXIO_LOG_DEFAULT_STREAM)
#else
#define AXIO_WARN(...) ((void)0)
#endif

#if AXIO_LOG_LEVEL >= AXIO_LOG_LEVEL_INFO
#define AXIO_INFO(...)                                                  \
  axio::output_log_header(AXIO_LOG_DEFAULT_STREAM, AXIO_LOG_LEVEL_INFO); \
  fprintf(AXIO_LOG_DEFAULT_STREAM, __VA_ARGS__);                        \
  fflush(AXIO_LOG_DEFAULT_STREAM)
#else
#define AXIO_INFO(...) ((void)0)
#endif

#if AXIO_LOG_LEVEL >= AXIO_LOG_LEVEL_REORDER
#define AXIO_REORDER(...)                                   \
  axio::output_log_header(axio_trace_file_or_default_stream, \
                         AXIO_LOG_LEVEL_REORDER);           \
  fprintf(axio_trace_file_or_default_stream, __VA_ARGS__);  \
  fflush(axio_trace_file_or_default_stream)
#else
#define AXIO_REORDER(...) ((void)0)
#endif

#if AXIO_LOG_LEVEL >= AXIO_LOG_LEVEL_TRACE
#define AXIO_TRACE(...)                                     \
  axio::output_log_header(axio_trace_file_or_default_stream, \
                         AXIO_LOG_LEVEL_TRACE);             \
  fprintf(axio_trace_file_or_default_stream, __VA_ARGS__);  \
  fflush(axio_trace_file_or_default_stream)
#else
#define AXIO_TRACE(...) ((void)0)
#endif

#if AXIO_LOG_LEVEL >= AXIO_LOG_LEVEL_CC
#define AXIO_CC(...)                                        \
  axio::output_log_header(axio_trace_file_or_default_stream, \
                         AXIO_LOG_LEVEL_CC);                \
  fprintf(axio_trace_file_or_default_stream, __VA_ARGS__);  \
  fflush(axio_trace_file_or_default_stream)
#else
#define AXIO_CC(...) ((void)0)
#endif

/// Return decent-precision time formatted as seconds:microseconds
static std::string get_formatted_time() {
  const auto now = std::chrono::high_resolution_clock::now();

  const size_t sec = static_cast<size_t>(
      std::chrono::time_point_cast<std::chrono::seconds>(now)
          .time_since_epoch()
          .count());

  const size_t usec = static_cast<size_t>(
      std::chrono::time_point_cast<std::chrono::microseconds>(now)
          .time_since_epoch()
          .count());

  // Roll-over seconds every 100 seconds
  char buf[20];
  snprintf(buf, sizeof(buf), "%zu:%06zu", sec % 100,
           (usec - (sec * 1000000)) /* spare microseconds */);
  return std::string(buf);
}

// Output log message header
static void output_log_header(FILE *stream, int level) {
  std::string formatted_time = get_formatted_time();

  const char *type;
  switch (level) {
    case AXIO_LOG_LEVEL_ERROR: type = "ERROR"; break;
    case AXIO_LOG_LEVEL_WARN: type = "WARNG"; break;
    case AXIO_LOG_LEVEL_INFO: type = "INFOR"; break;
    case AXIO_LOG_LEVEL_REORDER: type = "REORD"; break;
    case AXIO_LOG_LEVEL_TRACE: type = "TRACE"; break;
    case AXIO_LOG_LEVEL_CC: type = "CONGC"; break;
    default: type = "UNKWN";
  }

  fprintf(stream, "%s %s: ", formatted_time.c_str(), type);
}

/// Return true iff REORDER/TRACE/CC mode logging is disabled. These modes can
/// print an unreasonable number of log messages.
[[maybe_unused]] static bool is_log_level_reasonable() {
  return AXIO_LOG_LEVEL <= AXIO_LOG_LEVEL_INFO;
}

}  // namespace axio
