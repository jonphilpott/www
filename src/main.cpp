#include "httplib.h"
#include "generator.h"
#include "renderer.h"
#include "personas.h"

#include <string>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
#include <random>
#include <mutex>
#include <cctype>

// =============================================================================
// Argument parsing
// =============================================================================

struct Config {
    std::string model_path;
    int port         = 80;
    int n_threads    = 4;
    int n_ctx        = 4096;
    int n_gpu_layers = 0;
};

static void print_usage(const char* argv0) {
    fprintf(stderr,
        "Usage: %s --model <path.gguf> [options]\n"
        "\n"
        "Options:\n"
        "  --model      <path>   Path to GGUF model file (required)\n"
        "  --port       <n>      HTTP port to listen on (default: 80)\n"
        "  --threads    <n>      CPU threads for inference (default: 4)\n"
        "  --ctx        <n>      Context window size in tokens (default: 4096)\n"
        "  --gpu-layers <n>      Transformer layers to offload to GPU (default: 0)\n"
        "\n"
        "Example:\n"
        "  %s --model models/mistral-7b-instruct.gguf --port 8080 --threads 8\n",
        argv0, argv0);
}

static Config parse_args(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n", arg.c_str());
                exit(1);
            }
            return argv[++i];
        };
        if      (arg == "--model")      cfg.model_path   = next();
        else if (arg == "--port")       cfg.port         = std::stoi(next());
        else if (arg == "--threads")    cfg.n_threads    = std::stoi(next());
        else if (arg == "--ctx")        cfg.n_ctx        = std::stoi(next());
        else if (arg == "--gpu-layers") cfg.n_gpu_layers = std::stoi(next());
        else if (arg == "--help" || arg == "-h") { print_usage(argv[0]); exit(0); }
        else { fprintf(stderr, "Unknown argument: %s\n", arg.c_str()); print_usage(argv[0]); exit(1); }
    }
    if (cfg.model_path.empty()) {
        fprintf(stderr, "Error: --model is required\n\n");
        print_usage(argv[0]);
        exit(1);
    }
    return cfg;
}

// =============================================================================
// Prompt builders
// =============================================================================

// Builds the system + user prompt for the search results route.
// The model returns structured TITLE/URL/CATEGORY/DESC blocks which renderer.cpp parses.
static std::string search_system() {
    return "You generate fake 1990s website search results in a strict format. "
           "Output only the results, no introduction or explanation.";
}

static std::string search_user(const std::string& query) {
    return
        "Generate exactly 6 fake 1990s website search results for the query: \"" + query + "\"\n\n"
        "For each result output exactly these four fields on separate lines:\n"
        "TITLE: (enthusiastic 1990s-style page title)\n"
        "URL: (a fake path like /~username/page.html or /coolsite/facts.htm)\n"
        "CATEGORY: (Yahoo-style category like Entertainment > Movies or Science > Space)\n"
        "DESC: (one sentence describing the site, in 1990s web directory style)\n\n"
        "Separate each result with a line containing only three dashes: ---\n"
        "Output only the six results. No other text.";
}

// =============================================================================
// Single-stage page generation
//
// The system prompt carries the persona character + Geocities style rules.
// The user prompt asks for detailed, accurate content written directly as HTML.
// One inference pass — no re-processing of intermediate output, no context split.
//
// Key lesson from the two-stage experiment: the model doesn't need an example
// HTML page to produce the right aesthetic — describing the rules compactly in
// the system prompt is enough and leaves the full context budget for content.
// =============================================================================

// Combines persona character with concrete Geocities HTML rules.
// Describing style as rules rather than showing a full example page keeps the
// prompt short and leaves more room for actual content in the output.
static std::string page_system(const Persona& persona) {
    return
        persona.system_prompt + "\n\n"
        "You write 1990s Geocities-style HTML 3.2 web pages. "
        "Always use: a garish BODY BGCOLOR and TEXT color combination, "
        "FONT SIZE and COLOR tags throughout, CENTER tags, "
        "BLINK on at least one important phrase, a MARQUEE for the page heading, "
        "multiple HR horizontal rules as dividers, "
        "PRE tags around any code or preformatted text. "
        "Every page must end with: a fake visitor counter (e.g. Visitor #: 000847), "
        "the line 'Best viewed in Netscape Navigator 3.0 at 800x600', "
        "a fake last-updated date in 1997, "
        "and the author's ridiculous username with a mailto link.";
}

// topic  = the result title the user clicked on (e.g. "The Analog Guru")
// query  = the original search that produced it (e.g. "best analog synthesizers")
// Together they give the model enough context to write a focused, relevant page
// even when the title alone is ambiguous.
static std::string page_user(const std::string& topic, const std::string& query) {
    std::string subject = topic;
    if (!query.empty()) subject += " (context: found by searching for \"" + query + "\")";

    return
        "Write a Geocities HTML page about: " + subject + "\n\n"
        "Include detailed, accurate, genuinely useful information — "
        "code examples in <PRE> tags, step-by-step instructions, explanations, "
        "whatever someone learning about this topic would actually need. "
        "Add 2-3 invented 'facts' stated with total confidence, each wrapped !!like this!!. "
        "Output only HTML starting with <HTML>. No explanation.";
}

// =============================================================================
// Chat prompt builders
//
// A single dedicated "leet hacker" persona for the chat room.
// The critical design constraint: the persona is FLAVOUR, not SUBSTANCE.
// The model must always answer the question fully and correctly first;
// the 1337-speak and hacker attitude are applied on top, not instead.
//
// Small models need very explicit priority ordering in the system prompt —
// if you just say "be a character", they will be the character at the expense
// of being useful. The numbered rules below enforce a strict answer-first policy.
// =============================================================================

static std::string chat_system() {
    return
        "You are h4x0r_1337, an elite hacker and technical genius from the early internet "
        "era (circa 1995-1998). You have encyclopedic knowledge of C, C++, assembly, Unix, "
        "networking, algorithms, and all things technical.\n\n"
        "STRICT RULES — follow in this exact order:\n"
        "1. ALWAYS answer the question fully and accurately first. "
           "Never deflect, refuse, or give a wrong/incomplete answer. "
           "Giving bad information would be deeply n00bish and embarrassing.\n"
        "2. Wrap ALL code in triple backticks (``` ... ```) with the language name on the "
           "opening fence (e.g. ```c). Include complete, runnable code — no placeholders, "
           "no truncation, no omissions.\n"
        "3. After the technical content, add a brief sign-off in l33tspeak or hacker slang. "
           "Keep it short — one sentence maximum.\n"
        "4. Reference BBS, Usenet, IRC, and early internet culture naturally when relevant.\n\n"
        "The leet hacker attitude is seasoning, not a substitute for correct answers.";
}

// The user's raw message is passed straight through as the user turn.
static std::string chat_user(const std::string& msg) {
    return msg;
}

// =============================================================================
// PageStreamFilter — stateful post-processor for the /page streaming path.
//
// Sits between generate_stream() and the httplib DataSink. Applies the same
// logical fixes as render_content_page() but in a streaming-compatible way:
//
//   Phase 1 (PreambleHunt): buffer and discard until <html is found, so any
//     stray "Here is your page:" preamble from the model never reaches the browser.
//
//   Phase 2 (BodyHunt): buffer until the BODY opening tag closes (we see its ">"),
//     then call fix_color_contrast() on the accumulated header and flush it.
//     The BODY tag is typically in the first 100–200 chars, so the browser only
//     waits a fraction of a second before content starts arriving.
//
//   Phase 3 (Streaming): pass each character through a streaming
//     highlight_false_facts state machine and write to the sink immediately.
//
// fix_code_blocks() is intentionally not applied — it requires buffering complete
// <PRE>...</PRE> blocks which could span hundreds of tokens. The non-streaming
// render_content_page() path is unchanged and still applies all post-processing.
// =============================================================================

struct PageStreamFilter {

    httplib::DataSink& sink;

    enum class Phase { PreambleHunt, BodyHunt, Streaming };
    Phase phase = Phase::PreambleHunt;
    std::string buf;        // accumulates content during phases 1 and 2

    // Streaming highlight_false_facts state machine.
    // The regex-based version in renderer.cpp requires the full string. Here we
    // track state manually: buffer speculative '!' chars, emit a FONT wrapper
    // when we confirm a closing '!!'.
    std::string ff_pend;    // speculative chars (a lone '!' or in-fact content)
    bool        ff_in = false; // true while inside a !!...!! span

    explicit PageStreamFilter(httplib::DataSink& s) : sink(s) {}

    // Called for each token piece from generate_stream().
    // Returns false if the sink is no longer writable (client disconnected).
    bool process(const char* piece, int plen) {
        for (int i = 0; i < plen; i++) {
            if (!step(piece[i])) return false;
        }
        return sink.is_writable();
    }

    // Called after generation completes. Flushes any buffered content and
    // signals end-of-response to the httplib sink.
    void flush() {
        // Still in a buffering phase at EOS — emit whatever we have
        if (!buf.empty()) {
            sink.write(buf.c_str(), buf.size());
            buf.clear();
        }
        // Unclosed !!...!! span — emit the raw text rather than drop it
        if (!ff_pend.empty()) {
            sink.write(ff_pend.c_str(), ff_pend.size());
            ff_pend.clear();
        }
        sink.done();
    }

private:

    // Case-insensitive: does s end with needle? (needle must already be lowercase)
    static bool ends_ci(const std::string& s, const char* needle, size_t nlen) {
        if (s.size() < nlen) return false;
        size_t base = s.size() - nlen;
        for (size_t j = 0; j < nlen; j++) {
            if (std::tolower(static_cast<unsigned char>(s[base + j])) != needle[j])
                return false;
        }
        return true;
    }

    // Case-insensitive substring search. (needle must already be lowercase)
    static bool ci_has(const std::string& s, const char* needle, size_t nlen) {
        if (s.size() < nlen) return false;
        for (size_t i = 0; i <= s.size() - nlen; i++) {
            bool ok = true;
            for (size_t j = 0; j < nlen; j++) {
                if (std::tolower(static_cast<unsigned char>(s[i + j])) != needle[j])
                    { ok = false; break; }
            }
            if (ok) return true;
        }
        return false;
    }

    bool emit(const char* p, size_t n) { return n == 0 || sink.write(p, n); }
    bool emit(const std::string& s)    { return emit(s.c_str(), s.size()); }
    bool emit(char c)                  { return sink.write(&c, 1); }

    bool step(char c) {
        switch (phase) {
            case Phase::PreambleHunt: return step_preamble(c);
            case Phase::BodyHunt:     return step_body(c);
            case Phase::Streaming:    return step_stream(c);
        }
        return true;
    }

    // Buffer chars and discard everything before the first <html.
    bool step_preamble(char c) {
        buf += c;
        if (ends_ci(buf, "<html", 5)) {
            // Trim any preamble text that appeared before <html
            buf = buf.substr(buf.size() - 5);
            phase = Phase::BodyHunt;
        } else if (buf.size() >= 512) {
            // Safety cap: model started without preamble, just advance
            phase = Phase::BodyHunt;
        }
        return true;
    }

    // Buffer chars until the BODY opening tag's closing '>',
    // then fix color contrast on the accumulated header and flush it.
    bool step_body(char c) {
        buf += c;
        // The '>' is the signal to check — only evaluate when we might have a
        // complete BODY tag, because the regex in fix_color_contrast needs the tag.
        if (c == '>' && ci_has(buf, "<body", 5)) {
            std::string fixed = fix_color_contrast(buf);
            if (!emit(fixed)) return false;
            buf.clear();
            phase = Phase::Streaming;
        } else if (buf.size() >= 4096) {
            // Safety cap: flush and stream directly
            if (!emit(buf)) return false;
            buf.clear();
            phase = Phase::Streaming;
        }
        return true;
    }

    // Streaming !!false fact!! → <FONT COLOR="#FF0000"><I>...</I></FONT>.
    bool step_stream(char c) {
        if (!ff_in) {
            if (c == '!') {
                ff_pend += c;
                if (ff_pend.size() == 2) {   // confirmed opening !!
                    ff_in = true;
                    ff_pend.clear();
                }
            } else {
                if (!ff_pend.empty()) {       // lone '!' — flush it and continue
                    if (!emit(ff_pend)) return false;
                    ff_pend.clear();
                }
                if (!emit(c)) return false;
            }
        } else {
            ff_pend += c;
            size_t n = ff_pend.size();
            if (n >= 2 && ff_pend[n-2] == '!' && ff_pend[n-1] == '!') {
                std::string fact = ff_pend.substr(0, n - 2);
                std::string out  = "<FONT COLOR=\"#FF0000\"><I>" + fact + "</I></FONT>";
                if (!emit(out)) return false;
                ff_pend.clear();
                ff_in = false;
            }
        }
        return sink.is_writable();
    }
};

// =============================================================================
// Thread-safe persona picker
//
// std::rand() is not guaranteed to be thread-safe (the C++ standard says
// behaviour is implementation-defined under concurrent calls). httplib runs
// route handlers on a thread pool, so two /page requests could race.
// std::mt19937 with an explicit mutex is safe and has much better statistical
// properties than the C library's rand().
// =============================================================================

static std::mt19937 g_rng(static_cast<unsigned>(std::time(nullptr)));
static std::mutex   g_rng_mtx;

static size_t rand_persona_index() {
    std::lock_guard<std::mutex> lock(g_rng_mtx);
    // uniform_int_distribution gives an unbiased result — the modulo trick
    // used with rand() introduces subtle bias when RAND_MAX isn't a multiple
    // of the range, though it's negligible here.
    std::uniform_int_distribution<size_t> dist(0, PERSONAS.size() - 1);
    return dist(g_rng);
}

// =============================================================================
// main
// =============================================================================

int main(int argc, char* argv[]) {
    Config cfg = parse_args(argc, argv);

    // Nothing to do here — RNG is seeded at declaration below.

    // Load the model — this can take 5-30 seconds depending on model size and storage speed
    printf("Loading model: %s\n", cfg.model_path.c_str());
    Generator gen(cfg.model_path, cfg.n_ctx, cfg.n_threads, cfg.n_gpu_layers);
    if (!gen.is_ok()) {
        fprintf(stderr, "Failed to initialise generator. Check model path and available memory.\n");
        return 1;
    }
    printf("Model loaded. Starting server on port %d\n", cfg.port);

    httplib::Server svr;

    // Increase timeouts well beyond defaults — LLM inference can take 30-60 seconds.
    // Old browsers have their own timeouts but most will wait a minute or more.
    svr.set_read_timeout(300, 0);   // 5 minutes
    svr.set_write_timeout(300, 0);  // 5 minutes

    // -------------------------------------------------------------------------
    // Route: GET /  — Yahoo!-style home page with search form
    // -------------------------------------------------------------------------
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(render_home(), "text/html");
    });

    // -------------------------------------------------------------------------
    // Route: GET /search?q=<query>  — generate and display search results
    // -------------------------------------------------------------------------
    svr.Get("/search", [&gen](const httplib::Request& req, httplib::Response& res) {
        // httplib automatically URL-decodes query parameters
        std::string query = req.has_param("q") ? req.get_param_value("q") : "";
        query = query.empty() ? "the internet" : query;
        if (query.size() > 500) query.resize(500);  // prevent context window exhaustion

        printf("[search] query: %s\n", query.c_str());

        std::string raw = gen.generate(search_system(), search_user(query), 600, 0.7f);

        // Log the raw output so we can see exactly what the model returned
        printf("[search] raw output:\n---\n%s\n---\n", raw.c_str());

        res.set_content(render_search_results(query, raw), "text/html");
    });

    // -------------------------------------------------------------------------
    // Route: GET /chat?msg=<message>  — single-turn chat with a random persona
    //
    // No session state: each message is independent. A fresh persona is picked
    // every time, which means the "character" rotates with every reply — a
    // feature, not a bug, given the 90s chat-room aesthetic.
    // -------------------------------------------------------------------------
    svr.Get("/chat", [&gen](const httplib::Request& req, httplib::Response& res) {
        std::string msg = req.has_param("msg") ? req.get_param_value("msg") : "";
        if (msg.size() > 500) msg.resize(500);

        std::string response;
        std::string persona_name;

        if (!msg.empty()) {
            // Fixed leet-hacker persona — no random selection for chat.
            persona_name = "h4x0r_1337";
            printf("[chat] msg: %s\n", msg.c_str());

            // 1500 tokens matches the page generator — code answers need the headroom.
            // Temperature 0.7 keeps code output accurate while allowing some personality.
            response = gen.generate(chat_system(), chat_user(msg), 1500, 0.7f);
        }

        res.set_content(render_chat_page(msg, response, persona_name), "text/html");
    });

    // -------------------------------------------------------------------------
    // Route: GET /page?topic=<topic>  — generate a full Geocities content page
    //
    // A random persona is picked on each request — reloading a page gives a
    // different author, colour scheme and voice (intentional "always fresh" behaviour).
    // -------------------------------------------------------------------------
    svr.Get("/page", [&gen](const httplib::Request& req, httplib::Response& res) {
        std::string topic = req.has_param("topic") ? req.get_param_value("topic") : "the internet";
        std::string query = req.has_param("q")     ? req.get_param_value("q")     : "";
        if (topic.size() > 500) topic.resize(500);
        if (query.size() > 500) query.resize(500);

        const Persona& persona = PERSONAS[rand_persona_index()];
        printf("[page] topic: %s  query: %s  persona: %s\n",
               topic.c_str(), query.c_str(), persona.name.c_str());

        // Capture prompt strings by value — they are locals that go out of scope
        // when this route handler returns, before the content provider lambda runs
        // on httplib's worker thread.
        std::string sys = page_system(persona);
        std::string usr = page_user(topic, query);

        // Chunked transfer: the browser starts rendering immediately as tokens
        // arrive rather than waiting for the full 1500-token generation to finish.
        res.set_chunked_content_provider("text/html",
            [&gen, sys, usr](size_t offset, httplib::DataSink& sink) mutable -> bool {
                // httplib may call this provider more than once; only run on first call.
                if (offset == 0) {
                    PageStreamFilter filter(sink);
                    gen.generate_stream(sys, usr, 1500, 0.7f,
                        [&filter](const char* piece, int plen) -> bool {
                            return filter.process(piece, plen);
                        });
                    filter.flush();
                }
                sink.done();
                return true;
            });
    });

    // -------------------------------------------------------------------------
    // Route: GET /favicon.ico  — silence the inevitable 404 in server logs
    // -------------------------------------------------------------------------
    svr.Get("/favicon.ico", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;  // No Content — old browsers don't request favicons anyway
    });

    printf("Listening on http://0.0.0.0:%d — press Ctrl+C to stop\n", cfg.port);
    svr.listen("0.0.0.0", cfg.port);

    return 0;
}
