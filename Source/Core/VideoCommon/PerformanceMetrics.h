// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"
#include "VideoCommon/PerformanceTracker.h"

namespace Core
{
class System;
}

class PerformanceMetrics
{
public:
  PerformanceMetrics();
  ~PerformanceMetrics();

  PerformanceMetrics(const PerformanceMetrics&) = delete;
  PerformanceMetrics& operator=(const PerformanceMetrics&) = delete;
  PerformanceMetrics(PerformanceMetrics&&) = delete;
  PerformanceMetrics& operator=(PerformanceMetrics&&) = delete;

  void Reset();

  void CountFrame();
  void CountVBlank();

  // Call from CPU thread.
  void CountThrottleSleep(DT sleep);
  void CountPresentationSleep(DT sleep);
  void AdjustClockSpeed(s64 ticks, u32 new_ppc_clock, u32 old_ppc_clock);
  void CountPerformanceMarker(s64 ticks, u32 ticks_per_second);

  // Opt-in, low-overhead host phase timeline. Set MODERNGEKKO_FRAME_TIMELINE
  // to an output path before launch. GPU completion is asynchronous: these
  // methods must never introduce a wait into the render path.
  u64 RecordGpuSubmit();
  void RecordGpuComplete(u64 sequence, double gpu_start_seconds, double gpu_end_seconds,
                         u32 status);
  void RecordBackendPresent(DT duration, bool used_present_drawable);
  void RecordAudioCallback(DT work_duration, long requested_frames);

  // Getter Functions. May be called from any thread.
  double GetFPS() const;
  double GetVPS() const;
  double GetSpeed() const;
  double GetMaxSpeed() const;
  u32 GetEFBWidth() const;
  u32 GetEFBHeight() const;
  // Call from any thread.
  void SetLatestFramePresentationOffset(DT offset);

  // ImGui Functions
  void DrawImGuiStats(const float backbuffer_scale);

private:
  static u64 TimelineNowUS();
  void RecordSleepForTimeline(DT sleep, bool cpu_thread);
  void WriteTimelineLine(const std::string& line);

  PerformanceTracker m_fps_counter{"render_times.txt"};
  PerformanceTracker m_vps_counter{"vblank_times.txt"};

  double m_graph_max_time = 0.0;

  std::atomic<double> m_speed{};
  std::atomic<double> m_max_speed{};

  std::atomic<DT> m_frame_presentation_offset{};

  struct PerfSample
  {
    TimePoint clock_time;
    TimePoint work_time;
    s64 core_ticks;
  };

  std::deque<PerfSample> m_samples;
  DT m_time_sleeping{};

  bool m_timeline_enabled = false;
  std::ofstream m_timeline_file;
  std::mutex m_timeline_mutex;
  std::atomic<u64> m_timeline_present_sequence{};
  std::atomic<u64> m_timeline_vblank_sequence{};
  std::atomic<u64> m_timeline_gpu_sequence{};
  TimePoint m_timeline_last_present_time{};
  bool m_timeline_last_present_sane = false;
  std::atomic<u64> m_timeline_cpu_sleep_us{};
  std::atomic<u64> m_timeline_present_sleep_us{};
  std::atomic<u64> m_timeline_audio_callbacks{};
  std::atomic<u64> m_timeline_audio_work_us{};
  std::atomic<u64> m_timeline_audio_max_work_us{};
  std::atomic<u64> m_timeline_audio_max_gap_us{};
  std::atomic<u64> m_timeline_audio_last_callback_us{};
  u64 m_timeline_last_native_dispatches = 0;
  u64 m_timeline_last_native_bursts = 0;
  u64 m_timeline_last_native_cycles = 0;
  u64 m_timeline_last_idle_skips = 0;
  u64 m_timeline_last_fallback_entries = 0;

  Common::EventHook m_state_change_hook;
};
