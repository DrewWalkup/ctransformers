#include "llm.h"

// https://github.com/ggerganov/llama.cpp/blob/master/examples/main/main.cpp

#include "ggml/llama.cpp"

static void ct_llama_log_callback(llama_log_level level, const char *text,
                                  void *user_data) {}

class llama_llm : public LLM {
 public:
  llama_llm() { llama_log_set(ct_llama_log_callback, nullptr); }

  virtual ~llama_llm() {
    if (ctx_ != nullptr) {
      llama_free(ctx_);
    }
  }

  std::vector<gpt_vocab::id> Tokenize(const std::string &text,
                                      const bool add_bos_token) const override {
    const bool escape = llama_vocab_type(ctx_) == LLAMA_VOCAB_TYPE_SPM;
    return llama_tokenize_internal(ctx_->model.vocab, text, add_bos_token,
                                   escape);
  }

  const std::string &Detokenize(const gpt_vocab::id id) const override {
    if (id >= llama_n_vocab(ctx_)) {
      return kEmptyString;
    }
    detokenized_text_ = llama_token_to_text(ctx_, id);
    return detokenized_text_;
  }

  bool IsEosToken(const gpt_vocab::id token) const override {
    return token == EosToken();
  }

  gpt_vocab::id EosToken() const override { return llama_token_eos(ctx_); }

  gpt_vocab::id BosToken() const override { return llama_token_bos(ctx_); }

  int VocabSize() const override { return llama_n_vocab(ctx_); }

  const std::string &Architecture() const override {
    return LLM_ARCH_NAMES.at(ctx_->model.arch);
  }

  std::vector<float> &Logits() override { return ctx_->logits; }

  const std::vector<float> &Embeddings() const override {
    return ctx_->embedding;
  }

  gpt_vocab::id Sample(const int top_k, const float top_p,
                       const float temperature, const float repetition_penalty,
                       int last_n_tokens, int seed) const override {
    if (last_n_tokens < 0) {
      last_n_tokens = ContextLength();
    }
    if (seed < 0) {
      seed = time(nullptr);
    }
    ctx_->rng.seed(seed);

    const float *logits = llama_get_logits(ctx_);
    const int n_vocab = llama_n_vocab(ctx_);

    std::vector<llama_token_data> candidates;
    candidates.reserve(n_vocab);
    for (llama_token token_id = 0; token_id < n_vocab; token_id++) {
      candidates.emplace_back(
          llama_token_data{token_id, logits[token_id], 0.0f});
    }

    llama_token_data_array candidates_p = {
        candidates.data(),
        candidates.size(),
        false,
    };

    {
      std::unordered_set<gpt_vocab::id> recent_tokens_set;
      if (repetition_penalty != 1.0f) {
        recent_tokens_set = previous_tokens_.GetRecent(last_n_tokens);
      }
      std::vector<gpt_vocab::id> recent_tokens(recent_tokens_set.begin(),
                                               recent_tokens_set.end());
      llama_sample_repetition_penalty(ctx_, &candidates_p, recent_tokens.data(),
                                      recent_tokens.size(), repetition_penalty);
    }

    llama_sample_top_k(ctx_, &candidates_p, top_k, 1);
    llama_sample_top_p(ctx_, &candidates_p, top_p, 1);
    llama_sample_temperature(ctx_, &candidates_p, temperature);
    return llama_sample_token(ctx_, &candidates_p);
  }

 protected:
  bool Load(const std::string &filename, const Config &config) override {
    llama_context_params params = llama_context_default_params();
    params.embedding = true;
    if (config.context_length > 0) {
      params.n_ctx = config.context_length;
    }
    params.n_gpu_layers = config.gpu_layers;
    params.use_mmap = config.mmap;
    params.use_mlock = config.mlock;

    llama_model *model = llama_load_model_from_file(filename.c_str(), params);
    ctx_ = llama_new_context_with_model(model, params);
    if (ctx_ == nullptr) {
      return false;
    }
    ctx_->model_owner = true;
    n_ctx_ = llama_n_ctx(ctx_);
    return true;
  }

  bool Eval(const std::vector<gpt_vocab::id> &tokens, const int threads,
            const int n_past) override {
    const int status =
        llama_eval(ctx_, tokens.data(), tokens.size(), n_past, threads);
    return status == 0;
  }

 private:
  llama_context *ctx_ = nullptr;
  mutable std::string detokenized_text_;
};
