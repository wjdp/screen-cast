#include "web-socket-session.hpp"
#include "rgb2yuv.hpp"
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <sys/ipc.h>
#include <sys/shm.h>

WebSocketSession::WebSocketSession(tcp::socket socket) : ws(std::move(socket))
{
  initEncoder();
  initAudio();
}

WebSocketSession::~WebSocketSession()
{
  LOG("Destructor initiated");
  isRunning = false;

  if (paStream)
  {
    pa_simple_free(paStream);
    paStream = nullptr;
  }

  if (codecContext)
  {
    avcodec_free_context(&codecContext);
    codecContext = nullptr;
  }
  if (frame)
  {
    av_frame_free(&frame);
    frame = nullptr;
  }
  LOG("Destructor finished");
}

auto WebSocketSession::run(http::request<http::string_body> req) -> void
{
  LOG("Accept the WebSocket handshake");
  ws.accept(req);

  LOG("Start sending frames");
  startSendingFrames();
}

auto WebSocketSession::initEncoder() -> void
{
  LOG("Initialize FFmpeg encoder");

  // codec = avcodec_find_encoder_by_name("h264_nvenc");
  codec = avcodec_find_encoder(AV_CODEC_ID_H264);
  if (!codec)
  {
    LOG("Codec not found");
    exit(1);
  }

  codecContext = avcodec_alloc_context3(codec);
  if (!codecContext)
  {
    LOG("Could not allocate video codec context");
    exit(1);
  }

  codecContext->bit_rate = 6'000'000;
  codecContext->width = width;
  codecContext->height = height;
  codecContext->time_base = {1, 60};
  codecContext->framerate = {60, 1};
  codecContext->gop_size = 120;
  codecContext->max_b_frames = 0; // No B-frames
  codecContext->pix_fmt = AV_PIX_FMT_YUV420P;

  codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
  codecContext->thread_count = 0;

  av_opt_set(codecContext->priv_data, "preset", "ultrafast", 0);
  av_opt_set(codecContext->priv_data, "profile", "baseline", 0);
  av_opt_set(codecContext->priv_data, "tune", "zerolatency", 0);

  if (avcodec_open2(codecContext, codec, nullptr) < 0)
  {
    LOG("Could not open codec");
    exit(1);
  }

  frame = av_frame_alloc();
  if (!frame)
  {
    LOG("Could not allocate video frame");
    exit(1);
  }
  frame->format = codecContext->pix_fmt;
  frame->width = codecContext->width;
  frame->height = codecContext->height;

  if (const auto ret = av_frame_get_buffer(frame, 32); ret < 0)
  {
    LOG("Could not allocate the video frame data");
    exit(1);
  }
}

void WebSocketSession::initAudio()
{
  LOG("Initialize PulseAudio for audio capture");

  pa_sample_spec ss;
  ss.format = PA_SAMPLE_S16LE; // 16-bit PCM
  ss.rate = 48000;             // 48kHz sample rate
  ss.channels = 2;             // Stereo

  pa_buffer_attr buffer_attr;
  buffer_attr.maxlength = (uint32_t)-1; // Default maximum buffer size
  buffer_attr.tlength = (uint32_t)-1;   // Not used for recording
  buffer_attr.prebuf = (uint32_t)-1;    // Not used for recording
  buffer_attr.minreq = (uint32_t)-1;    // Default minimum request size
  buffer_attr.fragsize = 960;           // 0.005 seconds of audio (960 bytes)

  int error;
  paStream = pa_simple_new(NULL,                     // Use default server
                           "Screen Cast",            // Application name
                           PA_STREAM_RECORD,         // Stream direction (recording)
                           "@DEFAULT_SINK@.monitor", // Source to record from
                           "record",                 // Stream description
                           &ss,                      // Sample format specification
                           NULL,                     // Default channel map
                           &buffer_attr,             // Buffer attributes
                           &error                    // Error code
  );

  // paStream = pa_simple_new(NULL,             // Use default server
  //                          "Screen Cast",    // Application name
  //                          PA_STREAM_RECORD, // Stream direction (recording)
  //                          nullptr,          // Source to record from
  //                          "record",         // Stream description
  //                          &ss,              // Sample format specification
  //                          NULL,             // Default channel map
  //                          &buffer_attr,     // Buffer attributes
  //                          &error            // Error code
  // );
  if (!paStream)
  {
    LOG("pa_simple_new() failed:", pa_strerror(error));
    exit(1);
  }
}

auto WebSocketSession::startSendingFrames() -> void
{
  std::thread([self = shared_from_this()]() { self->videoThreadFunc(); }).detach();
  std::thread([self = shared_from_this()]() { self->audioThreadFunc(); }).detach();
}

auto WebSocketSession::videoThreadFunc() -> void
{
  const auto display = XOpenDisplay(nullptr);
  if (!display)
  {
    LOG("Cannot open display");
    return;
  }

  int displayHeight = DisplayHeight(display, 0);

  auto root = DefaultRootWindow(display);

  GLint att[] = {GLX_RGBA, GLX_DEPTH_SIZE, 24, GLX_DOUBLEBUFFER, None};
  XVisualInfo *vi = glXChooseVisual(display, 0, att);
  if (!vi)
  {
    LOG("No suitable visual found");
    XCloseDisplay(display);
    return;
  }

  GLXContext glc = glXCreateContext(display, vi, nullptr, GL_TRUE);
  if (!glc)
  {
    LOG("Cannot create OpenGL context");
    XCloseDisplay(display);
    return;
  }

  glXMakeCurrent(display, root, glc);

  auto rgb2yuv = Rgb2Yuv{8, width, height};

  unsigned char *pixels = (uint8_t *)std::aligned_alloc(32, width * height * 4); // Aligned to 32 bytes

  auto target = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000 / 60);
  while (isRunning)
  {
    const auto t1 = std::chrono::steady_clock::now();

    glReadBuffer(GL_FRONT);
    glReadPixels(x, displayHeight - height + y, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels);
    const auto cursorImage = XFixesGetCursorImage(display);
    if (cursorImage)
    {
      const auto cursorX = cursorImage->x - cursorImage->xhot - x;
      const auto cursorY = cursorImage->y - cursorImage->yhot - y;

      for (auto j = 0; j < cursorImage->height; ++j)
      {
        const auto imgY = cursorY + j;
        if (imgY < 0 || imgY >= height)
          continue;

        for (auto i = 0; i < cursorImage->width; ++i)
        {
          const auto imgX = cursorX + i;
          if (imgX < 0 || imgX >= width)
            continue;

          const auto cursorPixel = cursorImage->pixels[j * cursorImage->width + i];
          const auto alpha = (cursorPixel >> 24) & 0xff;
          if (alpha == 0)
            continue;

          const auto cr = static_cast<uint8_t>((cursorPixel >> 16) & 0xff);
          const auto cg = static_cast<uint8_t>((cursorPixel >> 8) & 0xff);
          const auto cb = static_cast<uint8_t>(cursorPixel & 0xff);

          const auto imageIndex = ((height - imgY) * width + imgX) * 3;

          const auto ir = pixels[imageIndex];
          const auto ig = pixels[imageIndex + 1];
          const auto ib = pixels[imageIndex + 2];

          const auto nr = static_cast<uint8_t>((cr * alpha + ir * (255 - alpha)) / 255);
          const auto ng = static_cast<uint8_t>((cg * alpha + ig * (255 - alpha)) / 255);
          const auto nb = static_cast<uint8_t>((cb * alpha + ib * (255 - alpha)) / 255);

          pixels[imageIndex] = nr;
          pixels[imageIndex + 1] = ng;
          pixels[imageIndex + 2] = nb;
        }
      }
      XFree(cursorImage);
    }

    const auto t2 = std::chrono::steady_clock::now();

    const auto src = reinterpret_cast<const uint8_t *>(pixels);
    const auto srcLineSize = width * 3;

    uint8_t *dst[3] = {frame->data[0], frame->data[1], frame->data[2]};
    int dstStride[3] = {frame->linesize[0], frame->linesize[1], frame->linesize[2]};
    rgb2yuv.convert(src, srcLineSize, dst, dstStride);

    const auto t3 = std::chrono::steady_clock::now();

    frame->pts = frameIndex++;

    if (encodeAndSendFrame() < 0)
    {
      LOG("Error encoding and sending frame");
      break;
    }
    const auto t4 = std::chrono::steady_clock::now();

    if (t4 > target)
    {
      LOG("Frame delayed",
          std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t4 - target),
          "grab",
          std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t2 - t1),
          "color conv",
          std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t3 - t2),
          "encode",
          std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t4 - t3));
      target = t4 + std::chrono::milliseconds(1000 / 60);
    }
    else
    {
      std::this_thread::sleep_for(target - t4);
      target += std::chrono::milliseconds(1000 / 60);
    }
  }

  free(pixels);

  glXDestroyContext(display, glc);
  XCloseDisplay(display);

  LOG("Video thread ended");
}

auto WebSocketSession::encodeAndSendFrame() -> int
{
  auto ret = avcodec_send_frame(codecContext, frame);
  if (ret < 0)
  {
    LOG("Error sending a frame for encoding");
    return ret;
  }

  auto pkt = av_packet_alloc();
  if (!pkt)
  {
    LOG("Could not allocate AVPacket");
    return -1;
  }

  while (ret >= 0)
  {
    ret = avcodec_receive_packet(codecContext, pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
    {
      av_packet_free(&pkt);
      return 0;
    }
    else if (ret < 0)
    {
      LOG("Error during encoding");
      av_packet_free(&pkt);
      return ret;
    }

    try
    {
      // Prepend message type byte (0x01 for video)
      std::vector<uint8_t> message;
      message.push_back(0x01); // Video data identifier
      message.insert(message.end(), pkt->data, pkt->data + pkt->size);

      auto lock = std::unique_lock{avMutex};
      ws.binary(true);
      ws.write(boost::asio::buffer(message));
    }
    catch (const std::exception &e)
    {
      LOG("WebSocket write error:", e.what());
      av_packet_unref(pkt);
      av_packet_free(&pkt);
      return -1;
    }

    av_packet_unref(pkt);
  }

  av_packet_free(&pkt);
  return 0;
}

auto WebSocketSession::audioThreadFunc() -> void
{
  const size_t bufferSize = 960 * 2 * sizeof(int16_t);
  uint8_t buffer[bufferSize];

  auto first = true;

  while (isRunning)
  {
    int error;
    if (pa_simple_read(paStream, buffer, sizeof(buffer), &error) < 0)
    {
      LOG("pa_simple_read() failed:", pa_strerror(error));
      break;
    }

    if (first)
    {
      first = false;
      LOG("first audio sample");
    }

    // Prepend message type byte (0x02 for audio)
    std::vector<uint8_t> message;
    message.push_back(0x02); // Audio data identifier
    message.insert(message.end(), buffer, buffer + sizeof(buffer));

    try
    {
      auto lock = std::unique_lock{avMutex};
      ws.binary(true);
      ws.write(boost::asio::buffer(message));
    }
    catch (const std::exception &e)
    {
      LOG("WebSocket write error (audio):", e.what());
      break;
    }
  }
  LOG("Audio thread ended");
}
