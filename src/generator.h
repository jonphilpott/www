#pragma once
#include <string>
#include <mutex>
#include <functional>

// Forward-declare the llama.cpp types so we don't pull in the whole header here.
// The full llama.h is only needed in generator.cpp where the implementation lives.
struct llama_model;
struct llama_context;

// Generator wraps the llama.cpp model and context into a single object.
// Create one at startup, keep it alive for the server's lifetime.
class Generator {
public:
    // Loads the model from a .gguf file and creates the inference context.
    //   model_path   : path to the .gguf model file
    //   n_ctx        : maximum token context window (prompt + generated text)
    //   n_threads    : CPU threads for inference
    //   n_gpu_layers : how many transformer layers to offload to GPU (0 = CPU only)
    Generator(const std::string& model_path, int n_ctx, int n_threads, int n_gpu_layers);
    ~Generator();

    // Runs a chat-style prompt through the model and returns the generated text.
    //   system_prompt : sets the model's persona/behaviour
    //   user_prompt   : the actual request (example page + topic instruction)
    //   max_tokens    : generation budget — search results need ~512, pages ~1024
    //   temperature   : 0.0 = deterministic, 1.0+ = creative/chaotic; 0.8 is a good default
    // Blocking version — collects all tokens and returns the complete string.
    std::string generate(const std::string& system_prompt,
                         const std::string& user_prompt,
                         int max_tokens,
                         float temperature = 0.8f);

    // Streaming version — calls on_token(piece, plen) for each token as it is
    // produced. The callback returns false to abort generation early (e.g. the
    // client disconnected). generate() is implemented as a wrapper around this.
    void generate_stream(const std::string& system_prompt,
                         const std::string& user_prompt,
                         int max_tokens,
                         float temperature,
                         std::function<bool(const char* piece, int plen)> on_token);

    bool is_ok() const { return model_ != nullptr && ctx_ != nullptr; }

    // Non-copyable — llama contexts own GPU/CPU memory and can't be duplicated
    Generator(const Generator&)            = delete;
    Generator& operator=(const Generator&) = delete;

private:
    llama_model*   model_ = nullptr;
    llama_context* ctx_   = nullptr;

    // llama_context is not thread-safe; this mutex serialises concurrent requests
    // so the server can still accept connections while one page is generating
    std::mutex mtx_;
};
