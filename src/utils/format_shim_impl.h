// license:GPLv3+
#pragma once

#if defined(__has_include_next)
#if __has_include_next(<format>)
#include_next <format>
#endif
#endif

#if !defined(__cpp_lib_format) || (__cpp_lib_format < 201907L)
#ifndef FMT_HEADER_ONLY
#define FMT_HEADER_ONLY
#endif
#include <fmt/format.h>
#include <fmt/xchar.h>
#include <string>
#include <utility>

namespace std
{
template <typename... Args>
inline std::string format(fmt::format_string<Args...> formatString, Args&&... args)
{
   return fmt::format(formatString, std::forward<Args>(args)...);
}

template <typename... Args>
inline std::string format(const std::string& formatString, Args&&... args)
{
   return fmt::vformat(formatString, fmt::make_format_args(args...));
}

template <typename... Args>
inline std::string format(const char* formatString, Args&&... args)
{
   return fmt::vformat(formatString, fmt::make_format_args(args...));
}

template <typename... Args>
inline std::wstring format(fmt::wformat_string<Args...> formatString, Args&&... args)
{
   return fmt::format(formatString, std::forward<Args>(args)...);
}

template <typename... Args>
inline std::wstring format(const std::wstring& formatString, Args&&... args)
{
   return fmt::vformat(formatString, fmt::make_wformat_args(args...));
}

template <typename... Args>
inline std::wstring format(const wchar_t* formatString, Args&&... args)
{
   return fmt::vformat(fmt::basic_string_view<wchar_t>(formatString), fmt::make_wformat_args(args...));
}
}
#endif