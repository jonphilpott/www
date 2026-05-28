#include "generator.h"
#include "llama.h"
#include <vector>
#include <cstdio>
#include <ctime>

// =============================================================================
// Constructor — loads model + creates context
// =============================================================================

Generator::Generator(const std::string& model_path, int n_ctx, int n_threads, int n_gpu_layers) {

    // Step 1: Initialise the llama.cpp backend.
    // This sets up GGML (the tensor library under llama.cpp), CPU threading,
    // and any GPU backends that were compiled in (CUDA, Metal, etc.).
    llama_backend_init();

    // Step 2: Load the model weights from disk.
    // .gguf files contain both the weights and model metadata (architecture,
    // vocabulary, chat template). n_gpu_layers controls how many transformer
    // layers are moved to VRAM; 0 keeps everything on CPU.
    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = n_gpu_layers;

    model_ = llama_model_load_from_file(model_path.c_str(), mparams);
    if (!model_) {
        fprintf(stderr, "[generator] Failed to load model: %s\n", model_path.c_str());
        return;
    }

    // Step 3: Create the inference context.
    // The context allocates the KV cache — the memory that stores the attention
    // state for every token in the current conversation. n_ctx is its capacity.
    // Larger values use more RAM but allow longer prompts + longer generations.
    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx        = static_cast<uint32_t>(n_ctx);
    cparams.n_threads    = static_cast<int32_t>(n_threads);
    // n_batch controls how many tokens are processed in parallel during the prefill
    // pass. The default (512) is conservative; 2048 processes the prompt faster,
    // especially noticeable on longer prompts like our page content.
    cparams.n_batch      = 2048;
    // Flash Attention replaces the standard O(n²) attention with a memory-efficient
    // fused kernel. Typically 20-40% faster on CPU and significantly faster on GPU.
    cparams.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;

    ctx_ = llama_init_from_model(model_, cparams);
    if (!ctx_) {
        fprintf(stderr, "[generator] Failed to create context\n");
        llama_model_free(model_);
        model_ = nullptr;
    }
}

// =============================================================================
// Destructor
// =============================================================================

Generator::~Generator() {
    // Order matters: free context before model (context holds a reference to model)
    if (ctx_)   llama_free(ctx_);
    if (model_) llama_model_free(model_);
    llama_backend_free();
}

// =============================================================================
// generate() — the main inference function
// =============================================================================

std::string Generator::generate(const std::string& system_prompt,
                                  const std::string& user_prompt,
                                  int max_tokens,
                                  float temperature) {

    // Only one request can use the context at a time
    std::lock_guard<std::mutex> lock(mtx_);

    // Grab the vocab — it's needed for tokenisation, token-to-text conversion,
    // and the EOS token. The vocab is owned by the model; don't free it separately.
    const llama_vocab* vocab = llama_model_get_vocab(model_);

    // -------------------------------------------------------------------------
    // Step 1: Apply the model's chat template.
    //
    // Different model families (Llama, Mistral, Qwen...) expect their prompts
    // wrapped in different special tokens and formatting. For example Llama 3
    // uses <|begin_of_text|><|start_header_id|>system<|end_header_id|>...
    // The .gguf file embeds the correct template; we fetch it with
    // llama_model_chat_template(), then pass it to llama_chat_apply_template().
    // -------------------------------------------------------------------------
    const char* tmpl = llama_model_chat_template(model_, nullptr);

    llama_chat_message msgs[] = {
        {"system", system_prompt.c_str()},
        {"user",   user_prompt.c_str()}
    };

    // First call with a modest buffer; resize and retry if the template is larger
    std::vector<char> tbuf(8192);
    int tlen = llama_chat_apply_template(tmpl, msgs, 2,
                                          /*add_ass=*/true,
                                          tbuf.data(), static_cast<int32_t>(tbuf.size()));
    if (tlen > static_cast<int>(tbuf.size())) {
        tbuf.resize(tlen);
        tlen = llama_chat_apply_template(tmpl, msgs, 2,
                                          true, tbuf.data(), static_cast<int32_t>(tbuf.size()));
    }
    if (tlen < 0) {
        return "<!-- generator error: chat template failed -->";
    }
    std::string formatted(tbuf.data(), tlen);

    // -------------------------------------------------------------------------
    // Step 2: Tokenise the formatted prompt.
    //
    // Tokenisation splits text into "tokens" — subword units the model
    // understands. "unbelievable" might become ["un", "believe", "able"].
    // The vocabulary is baked into the .gguf file.
    // -------------------------------------------------------------------------
    // Rough upper bound: ~4 chars per token on average, plus headroom
    std::vector<llama_token> tokens(formatted.size() / 2 + 64);
    int n_tokens = llama_tokenize(vocab,
                                   formatted.c_str(), static_cast<int32_t>(formatted.size()),
                                   tokens.data(), static_cast<int32_t>(tokens.size()),
                                   /*add_special=*/true, /*parse_special=*/true);
    if (n_tokens < 0) {
        // Negative return means the buffer was too small; resize and retry
        tokens.resize(-n_tokens + 16);
        n_tokens = llama_tokenize(vocab,
                                   formatted.c_str(), static_cast<int32_t>(formatted.size()),
                                   tokens.data(), static_cast<int32_t>(tokens.size()),
                                   true, true);
    }
    tokens.resize(n_tokens);

    // -------------------------------------------------------------------------
    // Step 3: Prefill pass — run the transformer over the entire prompt at once.
    //
    // llama_batch_get_one() creates a batch containing all prompt tokens.
    // llama_decode() runs the forward pass and populates the KV cache so the
    // model "remembers" everything it just read.
    // -------------------------------------------------------------------------
    llama_batch batch = llama_batch_get_one(tokens.data(), n_tokens);
    if (llama_decode(ctx_, batch) != 0) {
        llama_memory_clear(llama_get_memory(ctx_), true);
        return "<!-- generator error: prefill decode failed -->";
    }

    // -------------------------------------------------------------------------
    // Step 4: Build the sampler chain.
    //
    // After each forward pass the model outputs a probability distribution over
    // its entire vocabulary (~30k-100k tokens). The sampler decides which token
    // to actually pick. We stack three stages:
    //   top-p  : discard tokens outside the top 90% probability mass (reduces
    //            low-quality "long tail" choices)
    //   temp   : divide logits by temperature before softmax. Lower = more
    //            conservative choices, higher = more surprising/creative ones.
    //   dist   : sample from the resulting distribution using a seeded RNG.
    //            Using the current time as seed gives different output each call.
    // -------------------------------------------------------------------------
    llama_sampler* sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(sampler, llama_sampler_init_top_p(0.9f, 1));
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(temperature));
    llama_sampler_chain_add(sampler, llama_sampler_init_dist(static_cast<uint32_t>(std::time(nullptr))));

    // -------------------------------------------------------------------------
    // Step 5: Autoregressive generation loop.
    //
    // "Autoregressive" means each token is generated conditioned on all previous
    // tokens. We sample one token, append it to the output, feed it back as
    // the next input, and repeat until we hit the end-of-sequence token or
    // exhaust our budget. This is the inner loop that takes most of the time.
    // -------------------------------------------------------------------------
    std::string output;
    const llama_token eos = llama_vocab_eos(vocab);

    for (int i = 0; i < max_tokens; i++) {
        // Sample the next token from the current logits
        llama_token token = llama_sampler_sample(sampler, ctx_, -1);

        // llama_vocab_is_eog() catches all end-of-generation tokens (EOS, EOT, etc.)
        // which is more robust than checking for a single EOS token ID
        if (llama_vocab_is_eog(vocab, token)) break;

        // Convert the integer token ID back to its text fragment.
        // Tokens map to strings like "hello", " world", "\n", "<|endoftext|>".
        // lstrip=0 keeps leading spaces (important for word-boundary tokens).
        char piece[256];
        int plen = llama_token_to_piece(vocab, token, piece, sizeof(piece), 0, true);
        if (plen > 0) output.append(piece, plen);

        // Feed the new token back into the model for the next forward pass.
        // This single-token batch extends the KV cache by one position.
        llama_batch next = llama_batch_get_one(&token, 1);
        if (llama_decode(ctx_, next) != 0) break;
    }

    llama_sampler_free(sampler);

    // Clear the KV cache so the next request starts with a clean slate.
    // Without this, tokens from this conversation would "leak" into the next one.
    llama_memory_clear(llama_get_memory(ctx_), true);

    return output;
}
