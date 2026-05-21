#pragma once

#include "handle/log_handle.h"
#include "utils/source_location.h"

#define LOGGER_LOG(handle, level, message) \
    (handle).Log((level), LOGGER_SOURCE_LOCATION(), (message))

#define LOG_TRACE(handle, message)    LOGGER_LOG(handle, ::logger::LogLevel::trace,    message)
#define LOG_DEBUG(handle, message)    LOGGER_LOG(handle, ::logger::LogLevel::debug,    message)
#define LOG_INFO(handle, message)     LOGGER_LOG(handle, ::logger::LogLevel::info,     message)
#define LOG_WARN(handle, message)     LOGGER_LOG(handle, ::logger::LogLevel::warn,     message)
#define LOG_ERROR(handle, message)    LOGGER_LOG(handle, ::logger::LogLevel::error,    message)
#define LOG_CRITICAL(handle, message) LOGGER_LOG(handle, ::logger::LogLevel::critical, message)
