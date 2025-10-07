// First include stdlib headers
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>

// System includes
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <fcntl.h>

// Use atomic header with full path for problematic systems
#include <atomic>

// C standard library includes
extern "C" {
#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libswresample/swresample.h>
#include <stdio.h>
}

// Project includes
#include "common.h"
#include "cubeb/cubeb.h"
#include "cubeb_ringbuffer.h"
#include "ringbuffer.h"
#include "whisper.h"
#include <MacTypes.h>



wav_writer wavWriter;

void move_cursor(int row, int col) { printf("\033[%d;%dH", row, col); }

void clear_screen() { printf("\033[2J\033[H"); }

int term_width() {
  struct winsize w;
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
  return w.ws_col;
}

int term_height() {
  struct winsize w;
  ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
  return w.ws_col;
}

class TraceManager {
public:
  std::mutex mutex;
  std::ofstream out;
  bool first_event = true;

  TraceManager(const char *file) : out(file) { out << "[\n"; }

  ~TraceManager() { out << "\n]\n"; }

  void emitEvent(const std::string &json) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!first_event) {
      out << ",\n";
    }
    first_event = false;
    out << json;
    out.flush();
  }
};

class TraceEvent {
public:
  TraceEvent(TraceManager &manager, const std::string &name,
             const std::unordered_map<const char *, int> &args = {})
      : manager(manager), name(name) {
    start_time = std::chrono::steady_clock::now();
    tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    this->args = args;
  }

  ~TraceEvent() {
    auto end_time = std::chrono::steady_clock::now();
    auto start_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        start_time.time_since_epoch())
                        .count();
    auto dur_us = std::chrono::duration_cast<std::chrono::microseconds>(
                      end_time - start_time)
                      .count();

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);
    oss << "{\"name\": \"" << name << "\",";
    oss << " \"ph\": \"X\", \"ts\": " << start_us << ",";
    oss << " \"dur\": " << dur_us << ", \"pid\": 0, \"tid\": " << tid;

    if (!args.empty()) {
      oss << ", \"args\": {";
      bool first = true;
      for (const auto &[key, val] : args) {
        if (!first)
          oss << ", ";
        first = false;
        oss << "\"" << key << "\": " << val;
      }
      oss << "}";
    }
    oss << "}";

    manager.emitEvent(oss.str());
  }

private:
  TraceManager &manager;
  std::string name;
  std::chrono::steady_clock::time_point start_time;
  size_t tid;
  std::unordered_map<const char *, int> args;
};

class RTFPrinter {
public:
  RTFPrinter(const char *str, float dur_s) : str(str), dur_s(dur_s) {
    start_time = std::chrono::high_resolution_clock::now();
  }

  ~RTFPrinter() {
    auto duration = std::chrono::high_resolution_clock::now() - start_time;
    auto s = std::chrono::duration_cast<std::chrono::milliseconds>(duration)
                 .count() /
             1000.0;
    move_cursor(1, 50);
    printf("%s", std::string(term_width(), ' ').c_str());
    move_cursor(1, 50);
    printf("RTF: %.1fx (transcribed %.1fs in %.1fs)\n", dur_s / s, dur_s, s);
  }

private:
  const char *str;
  float dur_s;
  std::chrono::high_resolution_clock::time_point start_time;
};

TraceManager sManager("trace.json");

#define TRACE(str) TraceEvent trace(sManager, str);

#define TRACE_KV(str, k, v) TraceEvent trace(sManager, str, {{k, v}});

const double OUTPUT_SAMPLE_RATE = 16000.;
const int OUTPUT_CHANNELS = 1;
const AVSampleFormat OUTPUT_FORMAT = AV_SAMPLE_FMT_FLT;

typedef struct {
  AVFormatContext *fmt_ctx;
  AVCodecContext *dec_ctx;
  SwrContext *swr_ctx;
  int audio_stream_idx;
} AudioDecoder;

int init_decoder(const char *filename, AudioDecoder *decoder) {
  TRACE("init_decoder");
  avformat_open_input(&decoder->fmt_ctx, filename, NULL, NULL);
  avformat_find_stream_info(decoder->fmt_ctx, NULL);
  decoder->audio_stream_idx = av_find_best_stream(
      decoder->fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  AVStream *audio_stream = decoder->fmt_ctx->streams[decoder->audio_stream_idx];
  const AVCodec *dec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
  decoder->dec_ctx = avcodec_alloc_context3(dec);
  avcodec_parameters_to_context(decoder->dec_ctx, audio_stream->codecpar);
  avcodec_open2(decoder->dec_ctx, dec, NULL);

  AVChannelLayout out_ch_layout;
  av_channel_layout_default(&out_ch_layout, OUTPUT_CHANNELS);

  decoder->swr_ctx = swr_alloc();
  AVChannelLayout in_ch_layout = decoder->dec_ctx->ch_layout;

  swr_alloc_set_opts2(&decoder->swr_ctx, &out_ch_layout, OUTPUT_FORMAT,
                      OUTPUT_SAMPLE_RATE, &in_ch_layout,
                      decoder->dec_ctx->sample_fmt,
                      decoder->dec_ctx->sample_rate, 0, NULL);
  swr_init(decoder->swr_ctx);

  return 0;
}

int read_and_resample(AudioDecoder *decoder, float *output_buffer,
                      int output_samples) {
  TRACE("read_and_resample");
  AVPacket *pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  AVFrame *resampled = av_frame_alloc();

  av_channel_layout_default(&resampled->ch_layout, OUTPUT_CHANNELS);
  resampled->format = OUTPUT_FORMAT;
  resampled->sample_rate = OUTPUT_SAMPLE_RATE;
  resampled->nb_samples = output_samples;
  av_frame_get_buffer(resampled, 0);

  int samples_written = 0;
  while (samples_written < output_samples &&
         av_read_frame(decoder->fmt_ctx, pkt) >= 0) {
    if (pkt->stream_index != decoder->audio_stream_idx) {
      av_packet_unref(pkt);
      continue;
    }

    avcodec_send_packet(decoder->dec_ctx, pkt);
    while (avcodec_receive_frame(decoder->dec_ctx, frame) == 0) {
      int out_samples = swr_convert(
          decoder->swr_ctx, resampled->data, output_samples - samples_written,
          (const uint8_t **)frame->data, frame->nb_samples);

      memcpy(output_buffer + samples_written, resampled->data[0],
             out_samples * sizeof(float));
      samples_written += out_samples;
    }
    av_packet_unref(pkt);
  }

  av_frame_free(&frame);
  av_frame_free(&resampled);
  av_packet_free(&pkt);
  return samples_written;
}

void cleanup_decoder(AudioDecoder *decoder) {
  swr_free(&decoder->swr_ctx);
  avcodec_free_context(&decoder->dec_ctx);
  avformat_close_input(&decoder->fmt_ctx);
}

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr uint32_t CHANNELS = 1;
constexpr uint32_t FRAMES_PER_BUFFER = 1024;

int decode_file(const char *filename, int chunk_ms,
                audio_ring_buffer_base<float> *ringbuffer) {
  std::thread thread([filename, chunk_ms, ringbuffer]() mutable {
    int samples_per_chunk = (OUTPUT_SAMPLE_RATE * chunk_ms) / 1000;
    float *buffer = (float *)av_malloc(samples_per_chunk * sizeof(float));

    printf("Decoding in chunks of %d samples\n", samples_per_chunk);

    float t = 0;
    AudioDecoder decoder = {};
    init_decoder(filename, &decoder);
    int samples;
    while ((samples = read_and_resample(&decoder, buffer, samples_per_chunk)) >
           0) {
      ringbuffer->enqueue(buffer, samples);
      int duration_s = 1000. * samples / OUTPUT_SAMPLE_RATE;
      std::this_thread::sleep_for(std::chrono::milliseconds(duration_s));
      move_cursor(5, 0);
      printf("t = %.2f\n", t);
      t += duration_s / 1000.0f;
    }

    cleanup_decoder(&decoder);
    av_free(buffer);
  });

  thread.detach();
  return 0;
}

void display_vu_meter(const float *buffer, size_t length) {
  // Compute RMS
  float sum_squares = 0.0f;
  for (size_t i = 0; i < length; ++i) {
    sum_squares += buffer[i] * buffer[i];
  }
  float rms = std::sqrt(sum_squares / length);

  // Map RMS to meter length (0 to 50 chars)
  int meter_width = 50;
  int level = static_cast<int>(std::min(1.0f, rms) * meter_width);

  // Print VU meter
  std::cerr << "[";
  for (int i = 0; i < meter_width; ++i) {
    if (i < level)
      std::cerr << "#";
    else
      std::cerr << " ";
  }
  std::cerr << "] " << rms << std::endl;
}

void high_pass_filter(std::vector<float> &data, float cutoff,
                      float sample_rate) {
  const float rc = 1.0f / (2.0f * M_PI * cutoff);
  const float dt = 1.0f / sample_rate;
  const float alpha = dt / (rc + dt);

  float y = data[0];

  for (size_t i = 1; i < data.size(); i++) {
    y = alpha * (y + data[i] - data[i - 1]);
    data[i] = y;
  }
}

//  500 -> 00:05.000
// 6000 -> 01:00.000
std::string to_timestamp(int64_t t, bool comma) {
  int64_t msec = t * 10;
  int64_t hr = msec / (1000 * 60 * 60);
  msec = msec - hr * (1000 * 60 * 60);
  int64_t min = msec / (1000 * 60);
  msec = msec - min * (1000 * 60);
  int64_t sec = msec / 1000;
  msec = msec - sec * 1000;

  char buf[32];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d%s%03d", (int)hr, (int)min,
           (int)sec, comma ? "," : ".", (int)msec);

  return std::string(buf);
}

bool vad_simple(std::vector<float> &pcmf32, int sample_rate, int last_ms,
                float vad_thold, float freq_thold, bool verbose) {
  const int n_samples = pcmf32.size();
  const int n_samples_last = (sample_rate * last_ms) / 1000;

  if (n_samples_last >= n_samples) {
    // not enough samples - assume no speech
    return false;
  }

  if (freq_thold > 0.0f) {
    high_pass_filter(pcmf32, freq_thold, sample_rate);
  }

  float energy_all = 0.0f;
  float energy_last = 0.0f;

  for (int i = 0; i < n_samples; i++) {
    energy_all += fabsf(pcmf32[i]);

    if (i >= n_samples - n_samples_last) {
      energy_last += fabsf(pcmf32[i]);
    }
  }

  energy_all /= n_samples;
  energy_last /= n_samples_last;

  if (verbose) {
    fprintf(
        stderr,
        "%s: energy_all: %f, energy_last: %f, vad_thold: %f, freq_thold: %f\n",
        __func__, energy_all, energy_last, vad_thold, freq_thold);
  }

  if (energy_last > vad_thold * energy_all) {
    return false;
  }

  return true;
}

std::atomic<bool> running(true);

long data_callback(cubeb_stream * /*stream*/, void *user_ptr,
                   const void *input_buffer, void * /*output_buffer*/,
                   long nframes) {
  auto *ringbuffer =
      static_cast<lock_free_audio_ring_buffer<float> *>(user_ptr);

  int rv = ringbuffer->enqueue(
      const_cast<float *>(reinterpret_cast<const float *>(input_buffer)),
      nframes);
  TRACE_KV("data_callback", "frames", nframes);
  if (rv != nframes) {
    fprintf(stderr, "Failed to enqueue audio data\n");
    running = false;
    return nframes;
  }

  // printf("enqueued %ld frames\n", nframes);

  return nframes;
}

void state_callback(cubeb_stream * /*stream*/, void * /*user_ptr*/,
                    cubeb_state state) {
  if (state == CUBEB_STATE_ERROR) {
    std::cerr << "Stream error!" << std::endl;
    running = false;
  }
}

struct whisper_params {
  int32_t n_threads = std::min(4, (int32_t)std::thread::hardware_concurrency());
  int32_t step_ms = 3000;
  int32_t length_ms = 10000;
  int32_t keep_ms = 1000;
  int32_t capture_id = -1;
  int32_t max_tokens = 32;
  int32_t audio_ctx = 0;
  int32_t beam_size = -1;

  float vad_thold = 0.6f;
  float freq_thold = 100.0f;

  bool translate = false;
  bool no_fallback = false;
  bool print_special = false;
  bool no_context = true;
  bool no_timestamps = false;
  bool tinydiarize = false;
  bool save_audio = false; // save audio to wav file
  bool use_gpu = true;
  bool flash_attn = false;

  std::string language = "en";
  std::string model = "models/ggml-base.en.bin";
  std::string fname_out;
  std::string input_file;
};

void whisper_print_usage(int argc, char **argv, const whisper_params &params);

static bool whisper_params_parse(int argc, char **argv,
                                 whisper_params &params) {
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      whisper_print_usage(argc, argv, params);
      exit(0);
    } else if (arg == "-t" || arg == "--threads") {
      params.n_threads = std::stoi(argv[++i]);
    } else if (arg == "--step") {
      params.step_ms = std::stoi(argv[++i]);
    } else if (arg == "--length") {
      params.length_ms = std::stoi(argv[++i]);
    } else if (arg == "--keep") {
      params.keep_ms = std::stoi(argv[++i]);
    } else if (arg == "-c" || arg == "--capture") {
      params.capture_id = std::stoi(argv[++i]);
    } else if (arg == "-mt" || arg == "--max-tokens") {
      params.max_tokens = std::stoi(argv[++i]);
    } else if (arg == "-ac" || arg == "--audio-ctx") {
      params.audio_ctx = std::stoi(argv[++i]);
    } else if (arg == "-bs" || arg == "--beam-size") {
      params.beam_size = std::stoi(argv[++i]);
    } else if (arg == "-vth" || arg == "--vad-thold") {
      params.vad_thold = std::stof(argv[++i]);
    } else if (arg == "-fth" || arg == "--freq-thold") {
      params.freq_thold = std::stof(argv[++i]);
    } else if (arg == "-tr" || arg == "--translate") {
      params.translate = true;
    } else if (arg == "-nf" || arg == "--no-fallback") {
      params.no_fallback = true;
    } else if (arg == "-ps" || arg == "--print-special") {
      params.print_special = true;
    } else if (arg == "-kc" || arg == "--keep-context") {
      params.no_context = false;
    } else if (arg == "-l" || arg == "--language") {
      params.language = argv[++i];
    } else if (arg == "-m" || arg == "--model") {
      params.model = argv[++i];
    } else if (arg == "-f" || arg == "--file") {
      params.fname_out = argv[++i];
    } else if (arg == "-tdrz" || arg == "--tinydiarize") {
      params.tinydiarize = true;
    } else if (arg == "-sa" || arg == "--save-audio") {
      params.save_audio = true;
    } else if (arg == "-ng" || arg == "--no-gpu") {
      params.use_gpu = false;
    } else if (arg == "-fa" || arg == "--flash-attn") {
      params.flash_attn = true;
    } else if (arg == "-if" || arg == "--input-file") {
      params.input_file = argv[++i];
    }

    else {
      fprintf(stderr, "error: unknown argument: %s\n", arg.c_str());
      whisper_print_usage(argc, argv, params);
      exit(0);
    }
  }

  return true;
}

void whisper_print_usage(int /*argc*/, char **argv,
                         const whisper_params &params) {
  fprintf(stderr, "\n");
  fprintf(stderr, "usage: %s [options]\n", argv[0]);
  fprintf(stderr, "\n");
  fprintf(stderr, "options:\n");
  fprintf(stderr, "  -h,       --help          [default] show this help "
                  "message and exit\n");
  fprintf(stderr,
          "  -t N,     --threads N     [%-7d] number of threads to use during "
          "computation\n",
          params.n_threads);
  fprintf(
      stderr,
      "            --step N        [%-7d] audio step size in milliseconds\n",
      params.step_ms);
  fprintf(stderr,
          "            --length N      [%-7d] audio length in milliseconds\n",
          params.length_ms);
  fprintf(stderr,
          "            --keep N        [%-7d] audio to keep from previous step "
          "in ms\n",
          params.keep_ms);
  fprintf(stderr, "  -c ID,    --capture ID    [%-7d] capture device ID\n",
          params.capture_id);
  fprintf(stderr,
          "  -mt N,    --max-tokens N  [%-7d] maximum number of tokens per "
          "audio chunk\n",
          params.max_tokens);
  fprintf(stderr,
          "  -ac N,    --audio-ctx N   [%-7d] audio context size (0 - all)\n",
          params.audio_ctx);
  fprintf(stderr,
          "  -bs N,    --beam-size N   [%-7d] beam size for beam search\n",
          params.beam_size);
  fprintf(stderr,
          "  -vth N,   --vad-thold N   [%-7.2f] voice activity detection "
          "threshold\n",
          params.vad_thold);
  fprintf(stderr,
          "  -fth N,   --freq-thold N  [%-7.2f] high-pass frequency cutoff\n",
          params.freq_thold);
  fprintf(stderr,
          "  -tr,      --translate     [%-7s] translate from source language "
          "to english\n",
          params.translate ? "true" : "false");
  fprintf(stderr,
          "  -nf,      --no-fallback   [%-7s] do not use temperature fallback "
          "while decoding\n",
          params.no_fallback ? "true" : "false");
  fprintf(stderr, "  -ps,      --print-special [%-7s] print special tokens\n",
          params.print_special ? "true" : "false");
  fprintf(
      stderr,
      "  -kc,      --keep-context  [%-7s] keep context between audio chunks\n",
      params.no_context ? "false" : "true");
  fprintf(stderr, "  -l LANG,  --language LANG [%-7s] spoken language\n",
          params.language.c_str());
  fprintf(stderr, "  -m FNAME, --model FNAME   [%-7s] model path\n",
          params.model.c_str());
  fprintf(stderr, "  -f FNAME, --file FNAME    [%-7s] text output file name\n",
          params.fname_out.c_str());
  fprintf(stderr,
          "  -tdrz,    --tinydiarize   [%-7s] enable tinydiarize (requires a "
          "tdrz model)\n",
          params.tinydiarize ? "true" : "false");
  fprintf(
      stderr,
      "  -sa,      --save-audio    [%-7s] save the recorded audio to a file\n",
      params.save_audio ? "true" : "false");
  fprintf(stderr, "  -ng,      --no-gpu        [%-7s] disable GPU inference\n",
          params.use_gpu ? "false" : "true");
  fprintf(
      stderr,
      "  -fa,      --flash-attn    [%-7s] flash attention during inference\n",
      params.flash_attn ? "true" : "false");

  fprintf(stderr, "  -if,      --input-file [%s] input-file \n",
          params.input_file.c_str());
  fprintf(stderr, "\n");
}

int setup_whisper(int argc, char *argv[], whisper_params &params,
                  whisper_context **ctx) {
  if (whisper_params_parse(argc, argv, params) == false) {
    return 1;
  }

  params.keep_ms = std::min(params.keep_ms, params.step_ms);
  params.length_ms = std::max(params.length_ms, params.step_ms);

  const int n_samples_step = (1e-3 * params.step_ms) * WHISPER_SAMPLE_RATE;
  const int n_samples_len = (1e-3 * params.length_ms) * WHISPER_SAMPLE_RATE;
  const int n_samples_keep = (1e-3 * params.keep_ms) * WHISPER_SAMPLE_RATE;
  const int n_samples_30s = (1e-3 * 30000.0) * WHISPER_SAMPLE_RATE;

  const bool use_vad = n_samples_step <= 0; // sliding window mode uses VAD

  const int n_new_line =
      !use_vad ? std::max(1, params.length_ms / params.step_ms - 1)
               : 1; // number of steps to print new line

  params.no_timestamps = false;
  params.no_context |= use_vad;
  params.max_tokens = 0;

  // whisper init
  if (params.language != "auto" &&
      whisper_lang_id(params.language.c_str()) == -1) {
    fprintf(stderr, "error: unknown language '%s'\n", params.language.c_str());
    whisper_print_usage(argc, argv, params);
    exit(0);
  }

  struct whisper_context_params cparams = whisper_context_default_params();

  cparams.use_gpu = params.use_gpu;
  cparams.flash_attn = params.flash_attn;

  *ctx = whisper_init_from_file_with_params(params.model.c_str(), cparams);

  cparams.dtw_token_timestamps = true;
  cparams.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V3_TURBO;

  std::vector<float> pcmf32(n_samples_30s, 0.0f);
  std::vector<float> pcmf32_old;
  std::vector<float> pcmf32_new(n_samples_30s, 0.0f);

  std::vector<whisper_token> prompt_tokens;

  // print some info about the processing
  // {
  //   fprintf(stderr, "\n");
  //   if (!whisper_is_multilingual(*ctx)) {
  //     if (params.language != "en" || params.translate) {
  //       params.language = "en";
  //       params.translate = false;
  //       fprintf(stderr,
  //               "%s: WARNING: model is not multilingual, ignoring language
  //               and " "translation options\n",
  //               __func__);
  //     }
  //   }
  //   fprintf(
  //       stderr,
  //       "%s: processing %d samples (step = %.1f sec / len = %.1f sec / keep =
  //       "
  //       "%.1f sec), %d threads, lang = %s, task = %s, timestamps = %d ...\n",
  //       __func__, n_samples_step, float(n_samples_step) /
  //       WHISPER_SAMPLE_RATE, float(n_samples_len) / WHISPER_SAMPLE_RATE,
  //       float(n_samples_keep) / WHISPER_SAMPLE_RATE, params.n_threads,
  //       params.language.c_str(), params.translate ? "translate" :
  //       "transcribe", params.no_timestamps ? 0 : 1);

  //   if (!use_vad) {
  //     fprintf(stderr, "%s: n_new_line = %d, no_context = %d\n", __func__,
  //             n_new_line, params.no_context);
  //   } else {
  //     fprintf(stderr, "%s: using VAD, will transcribe on speech activity\n",
  //             __func__);
  //   }

  //   fprintf(stderr, "\n");
  // }

  int n_iter = 0;

  bool is_running = true;

  std::ofstream fout;
  if (params.fname_out.length() > 0) {
    fout.open(params.fname_out);
    if (!fout.is_open()) {
      fprintf(stderr, "%s: failed to open output file '%s'!\n", __func__,
              params.fname_out.c_str());
      return 1;
    }
  }

  // save wav file
  if (params.save_audio) {
    // Get current date/time for filename
    time_t now = time(0);
    char buffer[80];
    strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", localtime(&now));
    std::string filename = std::string(buffer) + ".wav";

    wavWriter.open(filename, WHISPER_SAMPLE_RATE, 16, 1);
  }

  return 0;
}

int whisper_loop(lock_free_audio_ring_buffer<float> &ringbuffer,
                 float segment_size_ms, float sample_rate,
                 whisper_params params, struct whisper_context *ctx) {
  std::vector<float> audio;
  std::vector<float> audio_new;
  size_t segment_size = (float)SAMPLE_RATE * segment_size_ms / 1000;
  audio.resize(segment_size);
  int n_iter = 0;

  auto t_last = std::chrono::high_resolution_clock::now();
  const auto t_start = t_last;

  const int n_samples_step = (1e-3 * params.step_ms) * SAMPLE_RATE;
  const int n_samples_len = (1e-3 * params.length_ms) * SAMPLE_RATE;
  const int n_samples_keep = (1e-3 * params.keep_ms) * SAMPLE_RATE;
  const int n_samples_30s = (1e-3 * 30000.0) * SAMPLE_RATE;

  RingBuffer<float> rb(params.length_ms / 1000., sample_rate);

  const bool use_vad = n_samples_step <= 0; // sliding window mode uses VAD

  const int n_new_line =
      !use_vad ? std::max(1, params.length_ms / params.step_ms - 1)
               : 1; // number of steps to print new line

  params.no_timestamps = false;
  params.no_context |= use_vad;
  params.max_tokens = 0;

  std::vector<float> pcmf32(n_samples_30s, 0.0f);
  std::vector<float> pcmf32_old;
  std::vector<float> pcmf32_new(n_samples_30s, 0.0f);
  std::vector<whisper_token> prompt_tokens;

  while (true) {
    // printf("In ringbuffer: %fms\n", ringbuffer.available_read() /
    // (float)sample_rate);
    if (ringbuffer.available_read() < n_samples_step) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    size_t read = ringbuffer.dequeue(pcmf32.data(), pcmf32.size());
    rb.enqueue(pcmf32.data(), read);

    // process new audio
    if (!use_vad) {
      while (true) {
        rb.get(params.step_ms, pcmf32_new);

        if ((int)pcmf32_new.size() > 2 * n_samples_step) {
          fprintf(stderr,
                  "\n\n%s: WARNING: cannot process audio fast enough, dropping "
                  "audio ...\n\n",
                  __func__);
          audio.clear();
          continue;
        }

        if ((int)pcmf32_new.size() >= n_samples_step) {
          audio.clear();
          break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }

      const int n_samples_new = pcmf32_new.size();

      // take up to params.length_ms audio from previous iteration
      const int n_samples_take =
          std::min((int)pcmf32_old.size(),
                   std::max(0, n_samples_keep + n_samples_len - n_samples_new));

      pcmf32.resize(n_samples_new + n_samples_take);

      for (int i = 0; i < n_samples_take; i++) {
        pcmf32[i] = pcmf32_old[pcmf32_old.size() - n_samples_take + i];
      }

      memcpy(pcmf32.data() + n_samples_take, pcmf32_new.data(),
             n_samples_new * sizeof(float));

      pcmf32_old = pcmf32;
    } else {
      const auto t_now = std::chrono::high_resolution_clock::now();
      const auto t_diff =
          std::chrono::duration_cast<std::chrono::milliseconds>(t_now - t_last)
              .count();

      if (t_diff < 2000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        continue;
      }

      rb.get(2000, pcmf32_new);

      if (::vad_simple(pcmf32_new, WHISPER_SAMPLE_RATE, 450, params.vad_thold,
                       params.freq_thold, false)) {
        rb.get(params.length_ms, pcmf32);
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }

      t_last = t_now;
    }

    // run the inference
    {
      whisper_full_params wparams = whisper_full_default_params(
          params.beam_size > 1 ? WHISPER_SAMPLING_BEAM_SEARCH
                               : WHISPER_SAMPLING_GREEDY);

      wparams.initial_prompt = "Talk by Paul Adenot on Web Codecs API";
      wparams.print_progress = false;
      wparams.print_special = params.print_special;
      wparams.print_realtime = false;
      wparams.print_timestamps = !params.no_timestamps;
      wparams.translate = params.translate;
      wparams.single_segment = !use_vad;
      wparams.max_tokens = params.max_tokens;
      wparams.language = params.language.c_str();
      wparams.n_threads = params.n_threads;
      wparams.beam_search.beam_size = params.beam_size;

      wparams.audio_ctx = params.audio_ctx;

      wparams.tdrz_enable = params.tinydiarize; // [TDRZ]

      // disable temperature fallback
      // wparams.temperature_inc  = -1.0f;
      wparams.temperature_inc =
          params.no_fallback ? 0.0f : wparams.temperature_inc;

      //  wparams.prompt_tokens =
      //    //params.no_context ? nullptr : prompt_tokens.data();
      //  wparams.prompt_n_tokens = params.no_context ? 0 : prompt_tokens.size();
      wparams.token_timestamps = true;

      // display_vu_meter(pcmf32.data(), pcmf32.size());
      //
      float marker[2] = {-1.0f, 1.0f};
      wavWriter.write(marker, 2);
      wavWriter.write(pcmf32.data(), pcmf32.size());
      {
        RTFPrinter rtf("whisper_full",
                       (float)pcmf32.size() / WHISPER_SAMPLE_RATE);
        // https://github.com/ggml-org/whisper.cpp/issues/137#issuecomment-1363301976
        // print long empty line to clear the previous line
        move_cursor(1, 1);
        printf("Transcribing from %.1f to %.1f\n", rb.clock(),
               rb.clock() + (float)pcmf32.size() / WHISPER_SAMPLE_RATE);
        if (whisper_full(ctx, wparams, pcmf32.data(), pcmf32.size()) != 0) {
          fprintf(stderr, "failed to process audio\n");
          return 6;
        }
      }

      // print result;
      {
        if (!use_vad) {
          printf("\33[2K\r");
          // print long empty line to clear the previous line
          printf("%s", std::string(100, ' ').c_str());
          printf("\33[2K\r");
        } else {
          const int64_t t1 = (t_last - t_start).count() / 1000000;
          const int64_t t0 =
              std::max(0.0, t1 - pcmf32.size() * 1000.0 / WHISPER_SAMPLE_RATE);

          printf("\n");
          printf("### Transcription %d START | t0 = %d ms | t1 = %d ms\n",
                 n_iter, (int)t0, (int)t1);
          printf("\n");
        }

        const int n_segments = whisper_full_n_segments(ctx);
        std::vector<whisper_token_data> tokens(n_segments);

        for (int i = 0; i < n_segments; ++i) {
          const char *text = whisper_full_get_segment_text(ctx, i);
          const size_t tokens_count = whisper_full_n_tokens(ctx, i);

          if (params.no_timestamps) {
            printf("%s", text);
            fflush(stdout);
          } else {
            // const int64_t t0 = whisper_full_get_segment_t0(ctx, i);
            // const int64_t t1 = whisper_full_get_segment_t1(ctx, i);

            // std::string output = "[" + to_timestamp(t0, false) + " --> " +
            //                      to_timestamp(t1, false) + "]  " + text;

            // if (whisper_full_get_segment_speaker_turn_next(ctx, i)) {
            //   output += " [SPEAKER_TURN]";
            // }

            // output += "\n";

            move_cursor(2, 0);
            printf("%s", std::string(term_width(), ' ').c_str());
            move_cursor(2, 0);
            printf("%s", text);
            move_cursor(3, 0);
            printf("%s", std::string(term_width(), ' ').c_str());
            move_cursor(4, 0);
            printf("%s", std::string(term_width(), ' ').c_str());
            int offset_left = 0;
            for (int j = 0; j < tokens_count; ++j) {
              char txt[100] = {};
              char txt_ts[100] = {};
              whisper_token_data tok = whisper_full_get_token_data(ctx, i, j);
              const char *tok_text = whisper_full_get_token_text(ctx, i, j);
              // skip special tokens
              if (tok_text[0] == '[') {
                continue;
              }
              int len_tok_tex = snprintf(txt, 100, "|%s", tok_text);
              int len_ts = snprintf(txt_ts, 100, "|%.1f,%.1f", tok.t0 / 100., tok.t1 / 100.);
              int width = std::max(len_ts, len_tok_tex);
              // width = std::min(offset_left + width, term_width() - offset_left);
              move_cursor(3, offset_left);
              printf("%s", txt);
              move_cursor(4, offset_left);
              printf("%s", txt_ts);
              offset_left += width;
            }
            printf("\n");

            fflush(stdout);
          }
        }

        if (use_vad) {
          printf("\n");
          printf("### Transcription %d END\n", n_iter);
        }
      }

      ++n_iter;

      if (!use_vad && (n_iter % n_new_line) == 0) {
        printf("\n");

        // keep part of the audio for next iteration to try to mitigate word
        // boundary issues
        pcmf32_old =
            std::vector<float>(pcmf32.end() - n_samples_keep, pcmf32.end());

        // Add tokens of the last full length segment as the prompt
        if (!params.no_context) {
          prompt_tokens.clear();

          const int n_segments = whisper_full_n_segments(ctx);
          for (int i = 0; i < n_segments; ++i) {
            const int token_count = whisper_full_n_tokens(ctx, i);
            for (int j = 0; j < token_count; ++j) {
              prompt_tokens.push_back(whisper_full_get_token_id(ctx, i, j));
            }
          }
        }
      }
      fflush(stdout);
    }
  }

  whisper_print_timings(ctx);
  whisper_free(ctx);

  return 0;
}

int init_cubeb(lock_free_audio_ring_buffer<float> *ringbuffer,
               whisper_params params) {
  cubeb *ctx = nullptr;
  cubeb_stream *stream = nullptr;
  cubeb_stream_params input_params;

  if (cubeb_init(&ctx, "Cubeb Audio Capture", nullptr) != CUBEB_OK) {
    std::cerr << "Failed to init cubeb" << std::endl;
    return 1;
  }

  cubeb_device_collection collection;
  if (cubeb_enumerate_devices(ctx, CUBEB_DEVICE_TYPE_INPUT, &collection) !=
      CUBEB_OK) {
    std::cerr << "Failed to enumerate input devices" << std::endl;
    cubeb_destroy(ctx);
    return 1;
  }

  for (uint32_t i = 0; i < collection.count; ++i) {
    const cubeb_device_info &dev = collection.device[i];
    std::cout << "Device " << i << ": " << dev.friendly_name << "\n";
    std::cout << "  Device ID: " << dev.device_id << "\n";
    std::cout << "  Group ID: " << dev.group_id << "\n";
    std::cout << "  Vendor: " << dev.vendor_name << "\n";
    std::cout << "  Type: "
              << ((dev.type & CUBEB_DEVICE_TYPE_INPUT) ? "Input" : "Unknown")
              << "\n";
    std::cout << "  State: "
              << ((dev.state == CUBEB_DEVICE_STATE_ENABLED) ? "Enabled"
                                                            : "Disabled")
              << "\n";
    std::cout << "  Preferred: "
              << ((dev.preferred & CUBEB_DEVICE_PREF_ALL) ? "Yes" : "No")
              << "\n";
    std::cout << "  Max channels: " << dev.max_channels << "\n";
    std::cout << "  Default rate: " << dev.default_rate << "\n";
    std::cout << "  Min rate: " << dev.min_rate
              << ", Max rate: " << dev.max_rate << "\n";
    std::cout << std::endl;
  }

  input_params.format = CUBEB_SAMPLE_FLOAT32NE;
  input_params.rate = SAMPLE_RATE;
  input_params.channels = CHANNELS;
  input_params.layout = CUBEB_LAYOUT_MONO;
  input_params.prefs = CUBEB_STREAM_PREF_NONE;

  if (cubeb_stream_init(ctx, &stream, "Capture Stream",
                        params.capture_id
                            ? collection.device[params.capture_id].device_id
                            : nullptr,
                        &input_params, nullptr, nullptr, // no output
                        FRAMES_PER_BUFFER, data_callback, state_callback,
                        &ringbuffer) != CUBEB_OK) {
    std::cerr << "Failed to init stream" << std::endl;
    cubeb_destroy(ctx);
    return 1;
  }

  if (cubeb_stream_start(stream) != CUBEB_OK) {
    std::cerr << "Failed to start stream" << std::endl;
    cubeb_stream_destroy(stream);
    cubeb_destroy(ctx);
    return 1;
  }
  return 0;
}

int main(int argc, char *argv[]) {
  whisper_params params;
  whisper_context *whisper_ctx = nullptr;
  int rv = setup_whisper(argc, argv, params, &whisper_ctx);

  // two segments
  lock_free_audio_ring_buffer<float> ringbuffer(1, 30 * SAMPLE_RATE);

  if (rv) {
    fprintf(stderr, "Setup whisper failed.\n");
    return rv;
  }

  if (params.input_file.empty()) {
    printf("Starting audio capture...\n");
    init_cubeb(&ringbuffer, params);
  } else {
    printf("Starting audio decoding...\n");
    decode_file(params.input_file.c_str(), 50, &ringbuffer);
  }

  for (int i = 0; i < term_height(); i++) {
    printf("\n");
  }
  whisper_loop(ringbuffer, params.step_ms, SAMPLE_RATE, params, whisper_ctx);

  return 0;
}
