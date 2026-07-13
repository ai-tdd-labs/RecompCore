// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinNoGUI/Platform.h"

#include <OptionParser.h>
#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#else
#include <Windows.h>
#endif

#include "Common/ScopeGuard.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Core.h"
#include "Core/DolphinAnalytics.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Core/Host.h"
#include "Core/Movie.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DiscIO/Enums.h"

#include "UICommon/CommandLineParse.h"
#ifdef USE_DISCORD_PRESENCE
#include "UICommon/DiscordPresence.h"
#endif
#include "UICommon/UICommon.h"
#include "VideoCommon/VideoEvents.h"

static std::unique_ptr<Platform> s_platform;

struct AutoFifoCapture
{
  std::string path;
  u64 start_presented_frame = 0;
  s32 frame_count = 0;
  std::string screenshot_name;
  bool stop_after_capture = false;
};

template <typename T>
static bool ParseUnsignedOption(const optparse::Values& options, const char* name, T* out)
{
  if (!options.is_set(name))
    return true;
  const std::string text = static_cast<const char*>(options.get(name));
  T parsed{};
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (error != std::errc{} || end != text.data() + text.size())
  {
    fprintf(stderr, "Invalid --%s value: %s\n", name, text.c_str());
    return false;
  }
  *out = parsed;
  return true;
}

static void signal_handler(int)
{
  constexpr char message[] = "A signal was received. A second signal will force Dolphin to stop.\n";
#ifdef _WIN32
  puts(message);
#else
  if (write(STDERR_FILENO, message, sizeof(message)) < 0)
  {
  }
#endif

  s_platform->RequestShutdown();
}

#ifndef _WIN32
// nogui has no hotkey scheduler; signals are the external savestate trigger
// (slot 1). The handlers only set atomic flags, consumed by the platform loop.
static void save_state_signal_handler(int)
{
  s_platform->RequestSaveState();
}

static void load_state_signal_handler(int)
{
  s_platform->RequestLoadState();
}
#endif

std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}

void Host_PPCSymbolsChanged()
{
}

void Host_PPCBreakpointsChanged()
{
}

bool Host_UIBlocksControllerState()
{
  return false;
}

void Host_Message(const HostMessageID id)
{
  if (id == HostMessageID::WMUserStop)
    s_platform->Stop();
}

void Host_UpdateTitle(const std::string& title)
{
  s_platform->SetTitle(title);
}

void Host_UpdateDisasmDialog()
{
}

void Host_JitCacheInvalidation()
{
}

void Host_JitProfileDataWiped()
{
}

void Host_RequestRenderWindowSize(int width, int height)
{
}

bool Host_RendererHasFocus()
{
  return s_platform->IsWindowFocused();
}

bool Host_RendererHasFullFocus()
{
  // Mouse capturing isn't implemented
  return Host_RendererHasFocus();
}

bool Host_RendererIsFullscreen()
{
  return s_platform->IsWindowFullscreen();
}

bool Host_TASInputHasFocus()
{
  return false;
}

void Host_YieldToUI()
{
}

void Host_TitleChanged()
{
#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateDiscordPresence();
#endif
}

void Host_UpdateDiscordClientID(const std::string& client_id)
{
#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateClientID(client_id);
#endif
}

bool Host_UpdateDiscordPresenceRaw(const std::string& details, const std::string& state,
                                   const std::string& large_image_key,
                                   const std::string& large_image_text,
                                   const std::string& small_image_key,
                                   const std::string& small_image_text,
                                   const int64_t start_timestamp, const int64_t end_timestamp,
                                   const int party_size, const int party_max)
{
#ifdef USE_DISCORD_PRESENCE
  return Discord::UpdateDiscordPresenceRaw(details, state, large_image_key, large_image_text,
                                           small_image_key, small_image_text, start_timestamp,
                                           end_timestamp, party_size, party_max);
#else
  return false;
#endif
}

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core> core)
{
  return nullptr;
}

static std::unique_ptr<Platform> GetPlatform(const optparse::Values& options)
{
  std::string platform_name = static_cast<const char*>(options.get("platform"));

#if HAVE_X11
  if (platform_name == "x11" || platform_name.empty())
    return Platform::CreateX11Platform();
#endif

#ifdef __linux__
  if (platform_name == "fbdev" || platform_name.empty())
    return Platform::CreateFBDevPlatform();
#endif

#ifdef _WIN32
  if (platform_name == "win32" || platform_name.empty())
    return Platform::CreateWin32Platform();
#endif
#ifdef __APPLE__
  if (platform_name == "macos" || platform_name.empty())
    return Platform::CreateMacOSPlatform();
#endif

  if (platform_name == "headless" || platform_name.empty())
    return Platform::CreateHeadlessPlatform();

  return nullptr;
}

#ifdef _WIN32
#define main app_main
#endif

int main(const int argc, char* argv[])
{
  const auto parser =
      CommandLineParse::CreateParser(CommandLineParse::ParserOptions::OmitGUIOptions);
  parser->add_option("-p", "--platform")
      .action("store")
      .help("Window platform to use [%choices]")
      .choices({"headless"
#ifdef __linux__
                ,
                "fbdev"
#endif
#if HAVE_X11
                ,
                "x11"
#endif
#ifdef _WIN32
                ,
                "win32"
#endif
#ifdef __APPLE__
                ,
                "macos"
#endif
      });
  parser->add_option("--fifo-record-path")
      .action("store")
      .help("Save an automated headless FIFO recording to this .dff path");
  parser->add_option("--fifo-record-start")
      .action("store")
      .help("Presented frames to skip before automated FIFO recording (default 0)");
  parser->add_option("--fifo-record-frames")
      .action("store")
      .help("Number of FIFO frames to record");
  parser->add_option("--fifo-record-stop")
      .action("store_true")
      .help("Stop emulation after the automated FIFO recording is saved");
  parser->add_option("--fifo-record-screenshot-name")
      .action("store")
      .help("Save a PNG after the automated FIFO window under ScreenShots/<game-id>");
  parser->add_option("--boot-gc-ipl")
      .action("store")
      .choices({"usa", "japan", "europe"})
      .metavar("<usa|japan|europe>")
      .help("Boot the installed GameCube IPL with no disc for the selected region");

  optparse::Values& options = CommandLineParse::ParseArguments(parser.get(), argc, argv);
  std::vector<std::string> args = parser->args();

  std::optional<AutoFifoCapture> auto_fifo;
  const bool any_fifo_option =
      options.is_set("fifo_record_path") || options.is_set("fifo_record_start") ||
      options.is_set("fifo_record_frames") || options.is_set("fifo_record_stop") ||
      options.is_set("fifo_record_screenshot_name");
  if (any_fifo_option)
  {
    if (!options.is_set("fifo_record_path") || !options.is_set("fifo_record_frames"))
    {
      fprintf(stderr, "--fifo-record-path and --fifo-record-frames are required together\n");
      return 1;
    }
    AutoFifoCapture capture;
    capture.path = static_cast<const char*>(options.get("fifo_record_path"));
    u32 frame_count = 0;
    if (capture.path.empty() ||
        !ParseUnsignedOption(options, "fifo_record_start", &capture.start_presented_frame) ||
        !ParseUnsignedOption(options, "fifo_record_frames", &frame_count) || frame_count == 0 ||
        frame_count > static_cast<u32>(std::numeric_limits<s32>::max()))
    {
      fprintf(stderr, "Invalid automated FIFO capture configuration\n");
      return 1;
    }
    capture.frame_count = static_cast<s32>(frame_count);
    if (options.is_set("fifo_record_screenshot_name"))
      capture.screenshot_name =
          static_cast<const char*>(options.get("fifo_record_screenshot_name"));
    capture.stop_after_capture = options.is_set("fifo_record_stop");
    auto_fifo = std::move(capture);
  }

  std::optional<std::string> save_state_path;
  if (options.is_set("save_state"))
  {
    save_state_path = static_cast<const char*>(options.get("save_state"));
  }

  std::unique_ptr<BootParameters> boot;
  std::optional<DiscIO::Region> gc_ipl_region;
  bool game_specified = false;
  if (options.is_set("boot_gc_ipl"))
  {
    if (options.is_set("exec") || options.is_set("nand_title") || !args.empty())
    {
      fprintf(stderr, "--boot-gc-ipl cannot be combined with a game or NAND title\n");
      return 1;
    }

    const std::string region = static_cast<const char*>(options.get("boot_gc_ipl"));
    if (region == "usa")
      gc_ipl_region = DiscIO::Region::NTSC_U;
    else if (region == "japan")
      gc_ipl_region = DiscIO::Region::NTSC_J;
    else
      gc_ipl_region = DiscIO::Region::PAL;
  }
  else if (options.is_set("exec"))
  {
    const std::list<std::string> paths_list = options.all("exec");
    const std::vector<std::string> paths{std::make_move_iterator(std::begin(paths_list)),
                                         std::make_move_iterator(std::end(paths_list))};
    boot = BootParameters::GenerateFromFile(
        paths, BootSessionData(save_state_path, DeleteSavestateAfterBoot::No));
    game_specified = true;
  }
  else if (options.is_set("nand_title"))
  {
    const std::string hex_string = static_cast<const char*>(options.get("nand_title"));
    if (hex_string.length() != 16)
    {
      fprintf(stderr, "Invalid title ID\n");
      parser->print_help();
      return 1;
    }
    const u64 title_id = std::stoull(hex_string, nullptr, 16);
    boot = std::make_unique<BootParameters>(BootParameters::NANDTitle{title_id});
  }
  else if (args.size())
  {
    boot = BootParameters::GenerateFromFile(
        args.front(), BootSessionData(save_state_path, DeleteSavestateAfterBoot::No));
    args.erase(args.begin());
    game_specified = true;
  }
  else
  {
    parser->print_help();
    return 0;
  }

  std::string user_directory;
  if (options.is_set("user"))
    user_directory = static_cast<const char*>(options.get("user"));

  s_platform = GetPlatform(options);
  if (!s_platform || !s_platform->Init())
  {
    fprintf(stderr, "No platform found, or failed to initialize.\n");
    return 1;
  }

  const WindowSystemInfo wsi = s_platform->GetWindowSystemInfo();

  UICommon::SetUserDirectory(user_directory);
  UICommon::Init();
  UICommon::InitControllers(wsi);

  Common::ScopeGuard ui_common_guard([] {
    UICommon::ShutdownControllers();
    UICommon::Shutdown();
  });

  if (gc_ipl_region)
    boot = std::make_unique<BootParameters>(BootParameters::IPL{*gc_ipl_region});

  if (save_state_path && !game_specified)
  {
    fprintf(stderr, "A save state cannot be loaded without specifying a game to launch.\n");
    return 1;
  }

  if (options.is_set("movie"))
  {
    std::optional<std::string> movie_save_state_path;
    const std::string movie_path = static_cast<const char*>(options.get("movie"));
    if (Core::System::GetInstance().GetMovie().PlayInput(movie_path, &movie_save_state_path))
    {
      if (movie_save_state_path)
        boot->boot_session_data.SetSavestateData(std::move(movie_save_state_path),
                                                 DeleteSavestateAfterBoot::No);
    }
    else
    {
      fprintf(stderr, "Could not play movie file: %s\n", movie_path.c_str());
      return 1;
    }
  }

  auto core_state_changed_hook = Core::AddOnStateChangedCallback([](const Core::State state) {
    if (state == Core::State::Uninitialized)
      s_platform->Stop();
  });

#ifdef _WIN32
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);
#else
  // Shut down cleanly on SIGINT and SIGTERM
  struct sigaction sa;
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART | SA_RESETHAND;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  // Savestate triggers (repeatable, so no SA_RESETHAND).
  struct sigaction sa_state;
  sigemptyset(&sa_state.sa_mask);
  sa_state.sa_flags = SA_RESTART;
  sa_state.sa_handler = save_state_signal_handler;
  sigaction(SIGUSR1, &sa_state, nullptr);
  sa_state.sa_handler = load_state_signal_handler;
  sigaction(SIGUSR2, &sa_state, nullptr);
#endif

  DolphinAnalytics::Instance().ReportDolphinStart("nogui");

  if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot), wsi))
  {
    fprintf(stderr, "Could not boot the specified file\n");
    return 1;
  }

  Common::EventHook auto_fifo_hook;
  u64 presented_frames = 0;
  std::atomic<u32> auto_fifo_stop_delay = 0;
  std::atomic<bool> auto_fifo_started = false;
  std::atomic<bool> auto_fifo_start_queued = false;
  if (auto_fifo)
  {
    const AutoFifoCapture capture = *auto_fifo;
    fprintf(stderr,
            "[auto-fifo] armed path=%s start_presented=%llu frames=%d screenshot=%s stop=%d\n",
            capture.path.c_str(), static_cast<unsigned long long>(capture.start_presented_frame),
            capture.frame_count,
            capture.screenshot_name.empty() ? "<none>" : capture.screenshot_name.c_str(),
            capture.stop_after_capture ? 1 : 0);

    const auto start_auto_fifo = [&auto_fifo_started,
                                  &auto_fifo_stop_delay](const AutoFifoCapture& pending) {
      if (auto_fifo_started.exchange(true))
        return;
      const char* parity_fast_forward = std::getenv("DOLPHIN_PARITY_FAST_FORWARD");
      const bool switch_to_interpreter =
          parity_fast_forward && parity_fast_forward[0] && parity_fast_forward[0] != '0';
      if (switch_to_interpreter)
      {
        Core::System& system = Core::System::GetInstance();
        Core::CPUThreadGuard guard(system);
        system.GetPowerPC().SetMode(PowerPC::CoreMode::Interpreter);
        fprintf(stderr, "[parity-oracle] fast-forward complete; interpreter window active\n");
      }
      fprintf(stderr, "[auto-fifo] start skipped_presented=%llu frames=%d mode=immediate\n",
              static_cast<unsigned long long>(pending.start_presented_frame),
              pending.frame_count);
      Core::System::GetInstance().GetFifoRecorder().StartRecording(
          pending.frame_count,
          [pending, &auto_fifo_stop_delay] {
            FifoDataFile* file = Core::System::GetInstance().GetFifoRecorder().GetRecordedFile();
            const bool saved = file != nullptr && file->Save(pending.path);
            fprintf(stderr, "[auto-fifo] complete saved=%d path=%s frames=%u\n", saved ? 1 : 0,
                    pending.path.c_str(), file != nullptr ? file->GetFrameCount() : 0u);
            Core::QueueHostJob([pending, &auto_fifo_stop_delay](Core::System&) {
              if (!pending.screenshot_name.empty())
              {
                Core::SaveScreenShot(pending.screenshot_name);
                fprintf(stderr, "[auto-fifo] screenshot requested name=%s\n",
                        pending.screenshot_name.c_str());
              }
              if (pending.stop_after_capture)
              {
                // Give FrameDumper at least one complete present after the
                // request before stopping the renderer.
                auto_fifo_stop_delay.store(pending.screenshot_name.empty() ? 1u : 2u);
              }
            });
          },
          true);
    };

    if (capture.start_presented_frame == 0)
      start_auto_fifo(capture);

    auto_fifo_hook = Core::System::GetInstance().GetVideoEvents().after_frame_event.Register(
        [capture, start_auto_fifo, &presented_frames, &auto_fifo_stop_delay,
         &auto_fifo_start_queued](const Core::System&) {
          const u32 stop_delay = auto_fifo_stop_delay.load();
          if (stop_delay != 0 && auto_fifo_stop_delay.fetch_sub(1) == 1)
          {
            Core::QueueHostJob([](Core::System& queued_system) { Core::Stop(queued_system); });
            return;
          }
          const u64 current = presented_frames++;
          if (capture.start_presented_frame == 0 ||
              current + 1 != capture.start_presented_frame)
            return;
          // Registering FifoRecorder's own after-frame hook from inside this
          // event dispatch leaves the new hook inactive on macOS. Queue the
          // recorder start onto the host job boundary so its callback is
          // installed before the next presentation dispatch.
          if (!auto_fifo_start_queued.exchange(true))
          {
            Core::QueueHostJob([capture, start_auto_fifo](Core::System&) {
              start_auto_fifo(capture);
            });
          }
        });
  }

#ifdef USE_DISCORD_PRESENCE
  Discord::UpdateDiscordPresence();
#endif

  s_platform->MainLoop();
  Core::Stop(Core::System::GetInstance());

  Core::Shutdown(Core::System::GetInstance());
  s_platform.reset();

  return 0;
}

#ifdef _WIN32
int wmain(int, wchar_t*[], wchar_t*[])
{
  std::vector<std::string> args = Common::CommandLineToUtf8Argv(GetCommandLineW());
  const int argc = static_cast<int>(args.size());
  std::vector<char*> argv(args.size());
  for (size_t i = 0; i < args.size(); ++i)
    argv[i] = args[i].data();

  return main(argc, argv.data());
}

#undef main
#endif
