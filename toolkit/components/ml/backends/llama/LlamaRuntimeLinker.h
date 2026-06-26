/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef LlamaRuntimeLinker_h_
#define LlamaRuntimeLinker_h_

#include "llama/llama.h"
#include "ggml.h"
#include "parakeet.h"
#include "parakeet_capi.h"
#include "whisper.h"

struct PRLibrary;

namespace mozilla::llama {

// Format: X(return_type, name, params)
#define MOZINFERENCE_FUNCTION_LIST(X)                                          \
  X(void, llama_log_set,                                                       \
    (void (*callback)(enum ggml_log_level, const char*, void*),                \
     void* user_data))                                                         \
  X(struct llama_model_params, llama_model_default_params, (void))             \
  X(struct llama_model*, llama_model_load_from_file_handle,                    \
    (FILE * file, struct llama_model_params params))                           \
  X(int32_t, llama_model_meta_val_str,                                         \
    (const struct llama_model* model, const char* key, char* buf,              \
     size_t buf_size))                                                         \
  X(struct llama_context_params, llama_context_default_params, (void))         \
  X(struct llama_context*, llama_init_from_model,                              \
    (struct llama_model * model, struct llama_context_params params))          \
  X(void, llama_attach_threadpool,                                             \
    (struct llama_context * ctx, ggml_threadpool_t threadpool,                 \
     ggml_threadpool_t threadpool_batch))                                      \
  X(const char*, llama_model_chat_template,                                    \
    (const struct llama_model* model, const char* name))                       \
  X(int32_t, llama_chat_apply_template,                                        \
    (const char* tmpl, const struct llama_chat_message* chat, size_t n_msg,    \
     bool add_ass, char* buf, int32_t length))                                 \
  X(struct llama_sampler_chain_params, llama_sampler_chain_default_params,     \
    (void))                                                                    \
  X(struct llama_sampler*, llama_sampler_chain_init,                           \
    (struct llama_sampler_chain_params params))                                \
  X(void, llama_sampler_chain_add,                                             \
    (struct llama_sampler * chain, struct llama_sampler * smpl))               \
  X(struct llama_sampler*, llama_sampler_init_greedy, (void))                  \
  X(struct llama_sampler*, llama_sampler_init_temp, (float t))                 \
  X(struct llama_sampler*, llama_sampler_init_dist, (uint32_t seed))           \
  X(struct llama_sampler*, llama_sampler_init_top_k, (int32_t k))              \
  X(struct llama_sampler*, llama_sampler_init_top_p,                           \
    (float p, size_t min_keep))                                                \
  X(struct llama_sampler*, llama_sampler_init_logit_bias,                      \
    (int32_t n_vocab, int32_t n_logit_bias,                                    \
     const llama_logit_bias* logit_bias))                                      \
  X(void, llama_memory_clear, (llama_memory_t mem, bool data))                 \
  X(llama_memory_t, llama_get_memory, (const struct llama_context* ctx))       \
  X(const struct llama_vocab*, llama_model_get_vocab,                          \
    (const struct llama_model* model))                                         \
  X(int32_t, llama_vocab_n_tokens, (const struct llama_vocab* vocab))          \
  X(int32_t, llama_tokenize,                                                   \
    (const struct llama_vocab* vocab, const char* text, int32_t text_len,      \
     llama_token* tokens, int32_t n_tokens_max, bool add_special,              \
     bool parse_special))                                                      \
  X(uint32_t, llama_n_ctx, (const struct llama_context* ctx))                  \
  X(struct llama_batch, llama_batch_get_one,                                   \
    (llama_token * tokens, int32_t n_tokens))                                  \
  X(int32_t, llama_memory_seq_pos_max,                                         \
    (llama_memory_t mem, llama_seq_id seq_id))                                 \
  X(int32_t, llama_decode,                                                     \
    (struct llama_context * ctx, struct llama_batch batch))                    \
  X(llama_token, llama_sampler_sample,                                         \
    (struct llama_sampler * smpl, struct llama_context * ctx, int32_t idx))    \
  X(bool, llama_vocab_is_eog,                                                  \
    (const struct llama_vocab* vocab, llama_token token))                      \
  X(int32_t, llama_token_to_piece,                                             \
    (const struct llama_vocab* vocab, llama_token token, char* buf,            \
     int32_t length, int32_t lstrip, bool special))                            \
  X(void, llama_model_free, (struct llama_model * model))                      \
  X(void, llama_free, (struct llama_context * ctx))                            \
  X(void, llama_sampler_free, (struct llama_sampler * smpl))                   \
  X(void, ggml_threadpool_params_init,                                         \
    (struct ggml_threadpool_params * p, int n_threads))                        \
  X(bool, ggml_threadpool_params_match,                                        \
    (const struct ggml_threadpool_params* p0,                                  \
     const struct ggml_threadpool_params* p1))                                 \
  X(ggml_threadpool_t, ggml_threadpool_new,                                    \
    (struct ggml_threadpool_params * params))                                  \
  X(void, ggml_threadpool_free, (ggml_threadpool_t threadpool))                \
  X(struct whisper_context_params, whisper_context_default_params, (void))     \
  X(struct whisper_context*, whisper_init_from_file_with_params,               \
    (const char* path_model, struct whisper_context_params params))            \
  X(struct whisper_context*, whisper_init_from_fd_with_params,                 \
    (int fd, struct whisper_context_params params))                            \
  X(struct whisper_full_params, whisper_full_default_params,                   \
    (enum whisper_sampling_strategy strategy))                                 \
  X(int, whisper_full,                                                         \
    (struct whisper_context * ctx, struct whisper_full_params params,          \
     const float* samples, int n_samples))                                     \
  X(int, whisper_full_n_segments, (struct whisper_context * ctx))              \
  X(const char*, whisper_full_get_segment_text,                                \
    (struct whisper_context * ctx, int i_segment))                             \
  X(int, whisper_full_n_tokens, (struct whisper_context * ctx, int i_segment)) \
  X(whisper_token, whisper_full_get_token_id,                                  \
    (struct whisper_context * ctx, int i_segment, int i_token))                \
  X(const char*, whisper_full_get_token_text,                                  \
    (struct whisper_context * ctx, int i_segment, int i_token))                \
  X(const char*, whisper_token_to_str,                                         \
    (struct whisper_context * ctx, whisper_token token))                       \
  X(whisper_token, whisper_token_eot, (struct whisper_context * ctx))          \
  X(whisper_token, whisper_token_beg, (struct whisper_context * ctx))          \
  X(void, whisper_free, (struct whisper_context * ctx))                        \
  X(struct parakeet_context_params, parakeet_context_default_params, (void))   \
  X(struct parakeet_context*, parakeet_init_from_fd_with_params,               \
    (int fd, struct parakeet_context_params params))                           \
  X(struct parakeet_full_params, parakeet_full_default_params,                 \
    (enum parakeet_sampling_strategy strategy))                                \
  X(int, parakeet_full,                                                        \
    (struct parakeet_context * ctx, struct parakeet_full_params params,        \
     const float* samples, int n_samples))                                     \
  X(int, parakeet_full_n_segments, (struct parakeet_context * ctx))            \
  X(const char*, parakeet_full_get_segment_text,                               \
    (struct parakeet_context * ctx, int i_segment))                            \
  X(int, parakeet_full_n_tokens,                                               \
    (struct parakeet_context * ctx, int i_segment))                            \
  X(parakeet_token, parakeet_full_get_token_id,                                \
    (struct parakeet_context * ctx, int i_segment, int i_token))               \
  X(struct parakeet_token_data, parakeet_full_get_token_data,                  \
    (struct parakeet_context * ctx, int i_segment, int i_token))               \
  X(const char*, parakeet_token_to_str,                                        \
    (struct parakeet_context * ctx, parakeet_token token))                     \
  X(int, parakeet_token_to_text,                                               \
    (const char* token_str, bool is_first, char* output, int max_len))         \
  X(void, parakeet_free, (struct parakeet_context * ctx))                      \
  /* mudler/parakeet.cpp cache-aware streaming C-API */                        \
  X(parakeet_ctx*, parakeet_capi_load_fd, (int fd))                            \
  X(void, parakeet_capi_free, (parakeet_ctx * ctx))                            \
  X(parakeet_stream*, parakeet_capi_stream_begin_lang,                         \
    (parakeet_ctx * ctx, const char* target_lang))                             \
  X(char*, parakeet_capi_stream_feed,                                          \
    (parakeet_stream * s, const float* pcm, int n_samples, int* eou_out))      \
  X(char*, parakeet_capi_stream_finalize, (parakeet_stream * s))               \
  X(int, parakeet_capi_stream_drain_words,                                     \
    (parakeet_stream * s, parakeet_stream_word** out_words))                   \
  X(void, parakeet_capi_free_words, (parakeet_stream_word * words, int count)) \
  X(void, parakeet_capi_stream_free, (parakeet_stream * s))                    \
  X(void, parakeet_capi_free_string, (char* s))

struct LlamaLibWrapper {
  LlamaLibWrapper() = default;
  ~LlamaLibWrapper() = default;

  enum class LinkResult {
    Success,
    NoProvidedLib,
    MissingFunction,
  };

  LinkResult Link();
  void Unlink();

  // Library handle
  PRLibrary* mLlamaLib;

#define DECLARE_FUNCTION_PTR(ret, name, params) ret(*name) params;
  MOZINFERENCE_FUNCTION_LIST(DECLARE_FUNCTION_PTR)
#undef DECLARE_FUNCTION_PTR
};

class LlamaRuntimeLinker {
 public:
  enum LinkStatus {
    LinkStatus_INIT = 0,
    LinkStatus_FAILED,
    LinkStatus_SUCCEEDED,
  };

  // Initialize the dynamic linker, returns true on success
  static bool Init();

  // Get the llama library wrapper
  static LlamaLibWrapper* Get() {
    if (!Init()) {
      return nullptr;
    }
    return &sLlamaLib;
  }

  // Check if the library has been successfully linked
  static bool IsAvailable() { return sLinkStatus == LinkStatus_SUCCEEDED; }

 private:
  static LlamaLibWrapper sLlamaLib;
  static LinkStatus sLinkStatus;
};

}  // namespace mozilla::llama

#endif  // LlamaRuntimeLinker_h_
