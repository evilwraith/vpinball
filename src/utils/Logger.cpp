// license:GPLv3+

#include <core/stdafx.h>
#include "Logger.h"

#include <sstream>
#include <iomanip>

#include <plog/Init.h>
#include <plog/Formatters/TxtFormatter.h>
#include <plog/Appenders/RollingFileAppender.h>
#ifdef __STANDALONE__
#ifndef __ANDROID__
#include <plog/Appenders/ColorConsoleAppender.h>
#else
#include <plog/Appenders/AndroidAppender.h>
#endif
#ifdef __LIBVPINBALL__
#include "lib/src/WebServer.h"
#endif
#endif

#include "core/VPApp.h"
#include "ui/win/codeview.h"
#include "ui/win/PinTableWnd.h"
#include "ui/win/WinEditor.h"


class DebugAppender final : public plog::IAppender
{
public:
   DebugAppender()
   {
      m_uiThreadId = std::this_thread::get_id();
   }

   void write(const plog::Record &record) PLOG_OVERRIDE
   {
      if ((std::this_thread::get_id() != m_uiThreadId) || (g_pvp == nullptr) || (g_pvp->GetActiveTableEditor() == nullptr))
         return;
      #ifdef _WIN32
      // Convert from wchar* to char* on Win32
      g_pvp->GetActiveTableEditor()->m_pcv->AddToDebugOutput(MakeString(record.getMessage()));
      #else
      g_pvp->GetActiveTableEditor()->m_pcv->AddToDebugOutput(record.getMessage());
      #endif
   }

private:
   std::thread::id m_uiThreadId;
};

#ifdef __LIBVPINBALL__
class WebServerAppender final : public plog::IAppender
{
public:
   void write(const plog::Record &record) PLOG_OVERRIDE
   {
      time_t rawTime = record.getTime().time;
      struct tm timeInfo;
      #ifdef _WIN32
      localtime_s(&timeInfo, &rawTime);
      #else
      localtime_r(&rawTime, &timeInfo);
      #endif

      std::string level;
      switch (record.getSeverity()) {
         case plog::fatal:   level = "FATAL"sv; break;
         case plog::error:   level = "ERROR"sv; break;
         case plog::warning: level = "WARN"sv; break;
         case plog::info:    level = "INFO"sv; break;
         case plog::debug:   level = "DEBUG"sv; break;
         case plog::verbose: level = "VERBOSE"sv; break;
         default:            level = "UNKNOWN"sv; break;
      }

      #ifdef _WIN32
      std::string message = MakeString(record.getMessage(), CP_UTF8);
      #else
      std::string message(record.getMessage());
      #endif

      char timeBuffer[32];
      snprintf(timeBuffer, std::size(timeBuffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
               timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
               timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec,
               static_cast<int>(record.getTime().millitm));

      std::stringstream ss;
      ss << timeBuffer << ' ' << std::left << std::setw(5) << level << ' ';
      ss << '[' << record.getTid() << "] ";
      ss << '[' << record.getFunc() << '@' << record.getLine() << "] ";
      ss << message;

      WebServer::LogAppender(ss.str());
   }
};
#endif

template <bool useUtcTime> class ThreadAwareTxtFormatter
{
public:
   static plog::util::nstring header() { return plog::util::nstring(); }

   static plog::util::nstring format(const plog::Record& record)
   {
      tm t;
      useUtcTime ? plog::util::gmtime_s(&t, &record.getTime().time) : plog::util::localtime_s(&t, &record.getTime().time);

      plog::util::nostringstream ss;
      ss << t.tm_year + 1900 << '-' << std::setfill(PLOG_NSTR('0')) << std::setw(2) << t.tm_mon + 1 << PLOG_NSTR('-') << std::setfill(PLOG_NSTR('0')) << std::setw(2) << t.tm_mday
         << PLOG_NSTR(' ');
      ss << std::setfill(PLOG_NSTR('0')) << std::setw(2) << t.tm_hour << PLOG_NSTR(':') << std::setfill(PLOG_NSTR('0')) << std::setw(2) << t.tm_min << PLOG_NSTR(':')
         << std::setfill(PLOG_NSTR('0')) << std::setw(2) << t.tm_sec << PLOG_NSTR('.') << std::setfill(PLOG_NSTR('0')) << std::setw(3) << static_cast<int>(record.getTime().millitm)
         << PLOG_NSTR(' ');
      ss << std::setfill(PLOG_NSTR(' ')) << std::setw(5) << std::left << severityToString(record.getSeverity()) << PLOG_NSTR(' ');
      #ifdef _WIN32
         bool logged = false;
         HANDLE hThread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, record.getTid());
         if (hThread != nullptr)
         {
            PWSTR data;
            HRESULT hr = GetThreadDescription(hThread, &data);
            if (SUCCEEDED(hr))
            {
               if (data[0] != 0)
               {
                  ss << PLOG_NSTR('[') << data << PLOG_NSTR("] ");
                  logged = true;
               }
               LocalFree(data);
            }
            CloseHandle(hThread);
         }
         if (!logged)
            ss << PLOG_NSTR('[') << record.getTid() << PLOG_NSTR("] ");
      #else
         ss << PLOG_NSTR('[') << record.getTid() << PLOG_NSTR("] ");
      #endif
      ss << PLOG_NSTR('[') << record.getFunc() << PLOG_NSTR('@') << record.getLine() << PLOG_NSTR("] ");
      ss << record.getMessage() << PLOG_NSTR('\n');

      return ss.str();
   }
};


#ifdef __RK3588__
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// Ported from the 10.8.0 fork (f999bf3f0): vpinball.log lives on the cart's ntfs-3g (fuseblk)
// mount, and plog's appenders write synchronously under a process-wide mutex -- every PLOG line is
// a userspace round-trip through the same FUSE daemon that streams PuP video off the same disk.
// Measured there as 60-80 ms frame stalls tracking log bursts. The formatted line is built on the
// calling thread (CPU-only, cheap); this formatter hands it to the downstream RollingFileAppender
// untouched so its rolling logic still applies.
struct PreformattedFormatter
{
   static plog::util::nstring header() { return plog::util::nstring(); }
   static plog::util::nstring format(const plog::Record& record) { return record.getMessage(); }
};

// Formats on the calling thread, queues the finished line, and lets one writer thread own the file
// and stdout. Bounded queue: when the disk stalls long enough to fill it, lines are DROPPED and
// counted rather than ever blocking a frame -- "nothing blocks the frame" outranks log
// completeness.
class AsyncLogAppender final : public plog::IAppender
{
public:
   AsyncLogAppender(plog::RollingFileAppender<PreformattedFormatter>* fileSink, const bool alsoStdout)
      : m_fileSink(fileSink)
      , m_stdout(alsoStdout)
      , m_thread(&AsyncLogAppender::ThreadMain, this)
   {
   }

   ~AsyncLogAppender() override
   {
      {
         std::lock_guard<std::mutex> guard(m_mutex);
         m_quit = true;
      }
      m_cv.notify_one();
      if (m_thread.joinable())
         m_thread.join();
   }

   void write(const plog::Record& record) PLOG_OVERRIDE
   {
      plog::util::nstring line = ThreadAwareTxtFormatter<false>::format(record);
      {
         std::lock_guard<std::mutex> guard(m_mutex);
         if (m_queue.size() >= kMaxQueuedLines)
         {
            ++m_dropped;
            return;
         }
         m_queue.emplace_back(std::move(line));
      }
      m_cv.notify_one();
   }

private:
   static constexpr size_t kMaxQueuedLines = 16384;

   void ThreadMain()
   {
      std::deque<plog::util::nstring> batch;
      for (;;)
      {
         size_t dropped = 0;
         {
            std::unique_lock<std::mutex> guard(m_mutex);
            m_cv.wait(guard, [this]() { return m_quit || !m_queue.empty(); });
            batch.swap(m_queue);
            dropped = m_dropped;
            m_dropped = 0;
            if (m_quit && batch.empty())
               return;
         }
         if (dropped > 0)
            batch.emplace_back("[AsyncLogAppender] dropped " + std::to_string(dropped) + " line(s) while the log disk was stalled\n");
         for (const plog::util::nstring& line : batch)
            Emit(line);
         if (m_stdout)
            fflush(stdout);
         batch.clear();
      }
   }

   void Emit(const plog::util::nstring& line)
   {
      // Rebuild a minimal Record whose message is the finished line; PreformattedFormatter passes
      // it straight through, keeping the file appender's size-based rolling.
      plog::Record record(plog::info, "", 0, "", nullptr, PLOG_DEFAULT_INSTANCE_ID);
      record << line;
      m_fileSink->write(record);
      if (m_stdout)
         fwrite(line.data(), 1, line.size(), stdout);
   }

   plog::RollingFileAppender<PreformattedFormatter>* const m_fileSink;
   const bool m_stdout;
   std::mutex m_mutex;
   std::condition_variable m_cv;
   std::deque<plog::util::nstring> m_queue;
   size_t m_dropped = 0;
   bool m_quit = false;
   std::thread m_thread;
};
#endif

Logger* Logger::m_pInstance = nullptr;

Logger* Logger::GetInstance()
{
   if (!m_pInstance)
      m_pInstance = new Logger();

   return m_pInstance;
}

void Logger::SetupLogger(const bool enable)
{
   plog::Severity maxLogSeverity = plog::none;
   if (enable)
   {
      static bool initialized = false;
      if (!initialized)
      {
         initialized = true;
         const std::filesystem::path logPath = g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Preferences, "vpinball.log");
#if defined(__RK3588__)
         // Both the file (ntfs-3g FUSE) and stdout are written by AsyncLogAppender's writer
         // thread; no game thread ever touches the disk for a log line. Construction order
         // matters: rawFileAppender must outlive fileAppender (function-local statics are
         // destroyed in reverse order), so the shutdown drain still has a live sink. 25 MB x2:
         // the launcher environment has been observed deleting vpinball.log.1 after exit, so the
         // primary file must be big enough for a whole session on its own.
         static plog::RollingFileAppender<PreformattedFormatter> rawFileAppender(logPath.string().c_str(), 1024 * 1024 * 25, 2);
         static AsyncLogAppender fileAppender(&rawFileAppender, true);
#elif PLOG_CHAR_IS_UTF8
         static plog::RollingFileAppender<ThreadAwareTxtFormatter<false>> fileAppender(logPath.string().c_str(), 1024 * 1024 * 5, 1);
#else
         static plog::RollingFileAppender<ThreadAwareTxtFormatter<false>> fileAppender(logPath.wstring().c_str(), 1024 * 1024 * 5, 1);
#endif
         static DebugAppender debugAppender;
         plog::Logger<PLOG_DEFAULT_INSTANCE_ID>::getInstance()->addAppender(&debugAppender);
         plog::Logger<PLOG_DEFAULT_INSTANCE_ID>::getInstance()->addAppender(&fileAppender);
         plog::Logger<PLOG_NO_DBG_OUT_INSTANCE_ID>::getInstance()->addAppender(&fileAppender);

#ifdef __STANDALONE__
#if defined(__RK3588__)
         // stdout is written by AsyncLogAppender's writer thread (colors dropped); a second,
         // synchronous console appender here would both duplicate every line and reintroduce a
         // blocking write on the calling thread when stdout is a slow pipe.
#elif !defined(__ANDROID__)
         static plog::ColorConsoleAppender<plog::TxtFormatter> consoleAppender;
         plog::Logger<PLOG_DEFAULT_INSTANCE_ID>::getInstance()->addAppender(&consoleAppender);
         plog::Logger<PLOG_NO_DBG_OUT_INSTANCE_ID>::getInstance()->addAppender(&consoleAppender);
#else
         static plog::AndroidAppender<plog::TxtFormatter> androidAppender("vpinball");
         plog::Logger<PLOG_DEFAULT_INSTANCE_ID>::getInstance()->addAppender(&androidAppender);
         plog::Logger<PLOG_NO_DBG_OUT_INSTANCE_ID>::getInstance()->addAppender(&androidAppender);
#endif
#ifdef __LIBVPINBALL__
         static WebServerAppender webServerAppender;
         plog::Logger<PLOG_DEFAULT_INSTANCE_ID>::getInstance()->addAppender(&webServerAppender);
         plog::Logger<PLOG_NO_DBG_OUT_INSTANCE_ID>::getInstance()->addAppender(&webServerAppender);
#endif  
#endif
      }
      #ifdef _DEBUG
      maxLogSeverity = plog::debug;
      #else
      maxLogSeverity = plog::info;
      #endif
   }
   plog::Logger<PLOG_DEFAULT_INSTANCE_ID>::getInstance()->setMaxSeverity(maxLogSeverity);
   plog::Logger<PLOG_NO_DBG_OUT_INSTANCE_ID>::getInstance()->setMaxSeverity(maxLogSeverity);
}

void Logger::Init()
{
   plog::init<PLOG_DEFAULT_INSTANCE_ID>();
   plog::init<PLOG_NO_DBG_OUT_INSTANCE_ID>(); // Logger that does not show in the debug window to avoid duplicated messages
}

void Logger::Truncate()
{
   std::filesystem::path szLogPath = g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Preferences, "vpinball.log");
   std::ofstream ofs(szLogPath, std::ofstream::out | std::ofstream::trunc);
   ofs.close();
}
