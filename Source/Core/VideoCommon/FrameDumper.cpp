// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/FrameDumper.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "Common/Assert.h"
#include "Common/FileUtil.h"
#include "Common/Image.h"

#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/Present.h"

// The video encoder needs the image to be a multiple of x samples.
static constexpr int VIDEO_ENCODER_LCM = 4;

namespace
{
FILE* s_parity_pixel_file = nullptr;
u32 s_parity_pixel_frame = 0;

bool IsParityPixelCaptureActive()
{
  const char* path = std::getenv("DOLPHIN_PARITY_PIXEL_FILE");
  return path && path[0] && OpcodeDecoder::g_record_fifo_data;
}

void CloseParityPixelFile()
{
  if (!s_parity_pixel_file)
    return;
  std::fflush(s_parity_pixel_file);
  std::fclose(s_parity_pixel_file);
  s_parity_pixel_file = nullptr;
}

void EmitParityPixelDigest(const u8* data, int width, int height, int stride)
{
  if (!IsParityPixelCaptureActive() || !data || width <= 0 || height <= 0 || stride < width * 4)
    return;
  if (!s_parity_pixel_file)
  {
    s_parity_pixel_file = std::fopen(std::getenv("DOLPHIN_PARITY_PIXEL_FILE"), "w");
    if (!s_parity_pixel_file)
      return;
    std::atexit(CloseParityPixelFile);
  }
  constexpr u64 fnv_basis = 1469598103934665603ull;
  constexpr u64 fnv_prime = 1099511628211ull;
  u64 hash = fnv_basis;
  for (int y = 0; y < height; ++y)
  {
    const u8* row = data + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width * 4; ++x)
      hash = (hash ^ row[x]) * fnv_prime;
  }
  ++s_parity_pixel_frame;
  std::fprintf(s_parity_pixel_file, "frame %u pixels %dx%d fnv %016llx\n",
               s_parity_pixel_frame, width, height,
               static_cast<unsigned long long>(hash));
  std::fflush(s_parity_pixel_file);

  const char* png_dir = std::getenv("DOLPHIN_PARITY_PIXEL_PNG_DIR");
  const char* png_limit_text = std::getenv("DOLPHIN_PARITY_PIXEL_PNG_LIMIT");
  const unsigned long png_limit = png_limit_text ? std::strtoul(png_limit_text, nullptr, 0) : 0;
  if (png_dir && png_dir[0] && s_parity_pixel_frame <= png_limit)
  {
    File::CreateFullPath(std::string(png_dir) + "/");
    char png_path[1024];
    std::snprintf(png_path, sizeof(png_path), "%s/frame_%06u.png", png_dir,
                  s_parity_pixel_frame);
    Common::ConvertRGBAToRGBAndSavePNG(png_path, data, width, height, stride,
                                      Config::Get(Config::GFX_PNG_COMPRESSION_LEVEL));
  }
}
}  // namespace

static bool DumpFrameToPNG(const FrameData& frame, const std::string& file_name)
{
  return Common::ConvertRGBAToRGBAndSavePNG(file_name, frame.data, frame.width, frame.height,
                                            frame.stride,
                                            Config::Get(Config::GFX_PNG_COMPRESSION_LEVEL));
}

FrameDumper::FrameDumper()
{
  m_frame_end_handle =
      GetVideoEvents().after_frame_event.Register([this](Core::System&) { FlushFrameDump(); });
}

FrameDumper::~FrameDumper()
{
  ShutdownFrameDumping();
}

void FrameDumper::DumpCurrentFrame(const AbstractTexture* src_texture,
                                   const MathUtil::Rectangle<int>& src_rect,
                                   const MathUtil::Rectangle<int>& target_rect, u64 ticks,
                                   int frame_number)
{
  int source_width = src_rect.GetWidth();
  int source_height = src_rect.GetHeight();
  int target_width = target_rect.GetWidth();
  int target_height = target_rect.GetHeight();

  // Parity capture compares the emulated video source, not the host window.
  // Keeping the source dimensions avoids a platform/backend-dependent stretch
  // (for example 640x480 to 640x511 on macOS) from changing every pixel hash.
  if (IsParityPixelCaptureActive())
  {
    target_width = source_width;
    target_height = source_height;
  }

  // We only need to render a copy if we need to stretch/scale the XFB copy.
  MathUtil::Rectangle<int> copy_rect = src_rect;
  if (source_width != target_width || source_height != target_height)
  {
    if (!CheckFrameDumpRenderTexture(target_width, target_height))
      return;

    g_gfx->ScaleTexture(m_frame_dump_render_framebuffer.get(),
                        m_frame_dump_render_framebuffer->GetRect(), src_texture, src_rect);
    src_texture = m_frame_dump_render_texture.get();
    copy_rect = src_texture->GetRect();
  }

  if (!CheckFrameDumpReadbackTexture(target_width, target_height))
    return;

  m_frame_dump_readback_texture->CopyFromTexture(src_texture, copy_rect, 0, 0,
                                                 m_frame_dump_readback_texture->GetRect());
  m_last_frame_state = m_ffmpeg_dump.FetchState(ticks, frame_number);
  m_frame_dump_needs_flush = true;
}

bool FrameDumper::CheckFrameDumpRenderTexture(u32 target_width, u32 target_height)
{
  // Ensure framebuffer exists (we lazily allocate it in case frame dumping isn't used).
  // Or, resize texture if it isn't large enough to accommodate the current frame.
  if (m_frame_dump_render_texture && m_frame_dump_render_texture->GetWidth() == target_width &&
      m_frame_dump_render_texture->GetHeight() == target_height)
  {
    return true;
  }

  // Recreate texture, but release before creating so we don't temporarily use twice the RAM.
  m_frame_dump_render_framebuffer.reset();
  m_frame_dump_render_texture.reset();
  m_frame_dump_render_texture = g_gfx->CreateTexture(
      TextureConfig(target_width, target_height, 1, 1, 1, AbstractTextureFormat::RGBA8,
                    AbstractTextureFlag_RenderTarget, AbstractTextureType::Texture_2DArray),
      "Frame dump render texture");
  if (!m_frame_dump_render_texture)
  {
    PanicAlertFmt("Failed to allocate frame dump render texture");
    return false;
  }
  m_frame_dump_render_framebuffer =
      g_gfx->CreateFramebuffer(m_frame_dump_render_texture.get(), nullptr);
  ASSERT(m_frame_dump_render_framebuffer);
  return true;
}

bool FrameDumper::CheckFrameDumpReadbackTexture(u32 target_width, u32 target_height)
{
  std::unique_ptr<AbstractStagingTexture>& rbtex = m_frame_dump_readback_texture;
  if (rbtex && rbtex->GetWidth() == target_width && rbtex->GetHeight() == target_height)
    return true;

  rbtex.reset();
  rbtex = g_gfx->CreateStagingTexture(StagingTextureType::Readback,
                                      TextureConfig(target_width, target_height, 1, 1, 1,
                                                    AbstractTextureFormat::RGBA8, 0,
                                                    AbstractTextureType::Texture_2DArray));
  if (!rbtex)
    return false;

  return true;
}

void FrameDumper::FlushFrameDump()
{
  if (!m_frame_dump_needs_flush)
    return;

  // Ensure dumping thread is done with output texture before swapping.
  FinishFrameData();

  std::swap(m_frame_dump_output_texture, m_frame_dump_readback_texture);

  // Queue encoding of the last frame dumped.
  auto& output = m_frame_dump_output_texture;
  output->Flush();
  if (output->Map())
  {
    DumpFrameData(reinterpret_cast<u8*>(output->GetMappedPointer()), output->GetConfig().width,
                  output->GetConfig().height, static_cast<int>(output->GetMappedStride()));
  }
  else
  {
    ERROR_LOG_FMT(VIDEO, "Failed to map texture for dumping.");
  }

  m_frame_dump_needs_flush = false;

  // Shutdown frame dumping if it is no longer active.
  if (!IsFrameDumping())
    ShutdownFrameDumping();
}

void FrameDumper::ShutdownFrameDumping()
{
  // Ensure the last queued readback has been sent to the encoder.
  FlushFrameDump();

  if (!m_frame_dump_thread_running.IsSet())
    return;

  // Ensure previous frame has been encoded.
  FinishFrameData();

  // Wake thread up, and wait for it to exit.
  m_frame_dump_thread_running.Clear();
  m_frame_dump_start.Set();
  if (m_frame_dump_thread.joinable())
    m_frame_dump_thread.join();
  m_frame_dump_render_framebuffer.reset();
  m_frame_dump_render_texture.reset();

  m_frame_dump_readback_texture.reset();
  m_frame_dump_output_texture.reset();
}

void FrameDumper::DumpFrameData(const u8* data, int w, int h, int stride)
{
  EmitParityPixelDigest(data, w, h, stride);
  m_frame_dump_data = FrameData{data, w, h, stride, m_last_frame_state};

  if (!m_frame_dump_thread_running.IsSet())
  {
    if (m_frame_dump_thread.joinable())
      m_frame_dump_thread.join();
    m_frame_dump_thread_running.Set();
    m_frame_dump_thread = std::thread(&FrameDumper::FrameDumpThreadFunc, this);
  }

  // Wake worker thread up.
  m_frame_dump_start.Set();
  m_frame_dump_frame_running = true;
}

void FrameDumper::FinishFrameData()
{
  if (!m_frame_dump_frame_running)
    return;

  m_frame_dump_done.Wait();
  m_frame_dump_frame_running = false;

  m_frame_dump_output_texture->Unmap();
}

void FrameDumper::FrameDumpThreadFunc()
{
  Common::SetCurrentThreadName("FrameDumping");

  bool dump_to_ffmpeg = !Config::Get(Config::GFX_DUMP_FRAMES_AS_IMAGES);
  bool frame_dump_started = false;

// If Dolphin was compiled without ffmpeg, we only support dumping to images.
#if !defined(HAVE_FFMPEG)
  if (dump_to_ffmpeg)
  {
    WARN_LOG_FMT(VIDEO, "FrameDump: Dolphin was not compiled with FFmpeg, using fallback option. "
                        "Frames will be saved as PNG images instead.");
    dump_to_ffmpeg = false;
  }
#endif

  while (true)
  {
    m_frame_dump_start.Wait();
    if (!m_frame_dump_thread_running.IsSet())
      break;

    auto frame = m_frame_dump_data;

    // Save screenshot
    if (m_screenshot_request.TestAndClear())
    {
      std::lock_guard<std::mutex> lk(m_screenshot_lock);

      if (DumpFrameToPNG(frame, m_screenshot_name))
        OSD::AddMessage("Screenshot saved to " + m_screenshot_name);

      // Reset settings
      m_screenshot_name.clear();
      m_screenshot_completed.Set();
    }

    if (Config::Get(Config::MAIN_MOVIE_DUMP_FRAMES))
    {
      if (!frame_dump_started)
      {
        if (dump_to_ffmpeg)
          frame_dump_started = StartFrameDumpToFFMPEG(frame);
        else
          frame_dump_started = StartFrameDumpToImage(frame);

        // Stop frame dumping if we fail to start.
        if (!frame_dump_started)
          Config::SetCurrent(Config::MAIN_MOVIE_DUMP_FRAMES, false);
      }

      // If we failed to start frame dumping, don't write a frame.
      if (frame_dump_started)
      {
        if (dump_to_ffmpeg)
          DumpFrameToFFMPEG(frame);
        else
          DumpFrameToImage(frame);
      }
    }

    m_frame_dump_done.Set();
  }

  if (frame_dump_started)
  {
    // No additional cleanup is needed when dumping to images.
    if (dump_to_ffmpeg)
      StopFrameDumpToFFMPEG();
  }
}

#if defined(HAVE_FFMPEG)

bool FrameDumper::StartFrameDumpToFFMPEG(const FrameData& frame)
{
  // If dumping started at boot, the start time must be set to the boot time to maintain audio sync.
  // TODO: Perhaps we should care about this when starting dumping in the middle of emulation too,
  // but it's less important there since the first frame to dump usually gets delivered quickly.
  const u64 start_ticks = frame.state.frame_number == 0 ? 0 : frame.state.ticks;
  return m_ffmpeg_dump.Start(frame.width, frame.height, start_ticks);
}

void FrameDumper::DumpFrameToFFMPEG(const FrameData& frame)
{
  m_ffmpeg_dump.AddFrame(frame);
}

void FrameDumper::StopFrameDumpToFFMPEG()
{
  m_ffmpeg_dump.Stop();
}

#else

bool FrameDumper::StartFrameDumpToFFMPEG(const FrameData&)
{
  return false;
}

void FrameDumper::DumpFrameToFFMPEG(const FrameData&)
{
}

void FrameDumper::StopFrameDumpToFFMPEG()
{
}

#endif  // defined(HAVE_FFMPEG)

std::string FrameDumper::GetFrameDumpNextImageFileName() const
{
  return fmt::format("{}framedump_{}.png", File::GetUserPath(D_DUMPFRAMES_IDX),
                     m_frame_dump_image_counter);
}

bool FrameDumper::StartFrameDumpToImage(const FrameData&)
{
  m_frame_dump_image_counter = 1;
  if (!Config::Get(Config::MAIN_MOVIE_DUMP_FRAMES_SILENT))
  {
    // Only check for the presence of the first image to confirm overwriting.
    // A previous run will always have at least one image, and it's safe to assume that if the user
    // has allowed the first image to be overwritten, this will apply any remaining images as well.
    std::string filename = GetFrameDumpNextImageFileName();
    if (File::Exists(filename))
    {
      if (!AskYesNoFmtT("Frame dump image(s) '{0}' already exists. Overwrite?", filename))
        return false;
    }
  }

  return true;
}

void FrameDumper::DumpFrameToImage(const FrameData& frame)
{
  DumpFrameToPNG(frame, GetFrameDumpNextImageFileName());
  m_frame_dump_image_counter++;
}

void FrameDumper::SaveScreenshot(std::string filename)
{
  std::lock_guard<std::mutex> lk(m_screenshot_lock);
  m_screenshot_name = std::move(filename);
  m_screenshot_request.Set();
}

bool FrameDumper::IsFrameDumping() const
{
  if (IsParityPixelCaptureActive())
    return true;
  if (m_screenshot_request.IsSet())
    return true;

  if (Config::Get(Config::MAIN_MOVIE_DUMP_FRAMES))
    return true;

  return false;
}

int FrameDumper::GetRequiredResolutionLeastCommonMultiple() const
{
  if (Config::Get(Config::MAIN_MOVIE_DUMP_FRAMES))
    return VIDEO_ENCODER_LCM;
  return 1;
}

void FrameDumper::DoState(PointerWrap& p)
{
#ifdef HAVE_FFMPEG
  m_ffmpeg_dump.DoState(p);
#endif
}
std::unique_ptr<FrameDumper> g_frame_dumper;
