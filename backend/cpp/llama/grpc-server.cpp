// llama.cpp gRPC C++ backend server
//
// Ettore Di Giacinto <mudler@localai.io> and llama.cpp authors
//
// This is a gRPC server for llama.cpp compatible with the LocalAI proto
// Note: this is a re-adaptation of the original llama.cpp example/server.cpp for HTTP (https://github.com/ggerganov/llama.cpp/tree/master/examples/server), 
// but modified to work with gRPC
//

#include <iostream>
#include <memory>
#include <string>
#include <getopt.h>
#include "../llava/clip.h"
#include "stb_image.h"
#include "common.h"
#include "json.hpp"
#include "llama.h"
#include "grammar-parser.h"
#include "backend.pb.h"
#include "backend.grpc.pb.h"

// include std::regex
#include <cstddef>
#include <thread>
#include <mutex>
#include <chrono>
#include <regex>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;


using backend::HealthMessage;


///// LLAMA.CPP server code below

using json = nlohmann::json;

static bool server_verbose = false;

#if SERVER_VERBOSE != 1
#define LOG_VERBOSE(MSG, ...)
#else
#define LOG_VERBOSE(MSG, ...)                                            \
    do                                                                   \
    {                                                                    \
        if (server_verbose)                                              \
        {                                                                \
            server_log("VERBOSE", __func__, __LINE__, MSG, __VA_ARGS__); \
        }                                                                \
    } while (0)
#endif

#define LOG_ERROR(  MSG, ...) server_log("ERROR",   __func__, __LINE__, MSG, __VA_ARGS__)
#define LOG_WARNING(MSG, ...) server_log("WARNING", __func__, __LINE__, MSG, __VA_ARGS__)
#define LOG_INFO(   MSG, ...) server_log("INFO",    __func__, __LINE__, MSG, __VA_ARGS__)

//
// base64 utils (TODO: move to common in the future)
//

static const std::string base64_chars =
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "abcdefghijklmnopqrstuvwxyz"
             "0123456789+/";

static inline bool is_base64(uint8_t c)
{
    return (isalnum(c) || (c == '+') || (c == '/'));
}

static std::vector<uint8_t> base64_decode(std::string const &encoded_string)
{
    int i = 0;
    int j = 0;
    int in_ = 0;

    int in_len = encoded_string.size();

    uint8_t char_array_4[4];
    uint8_t char_array_3[3];

    std::vector<uint8_t> ret;

    while (in_len-- && (encoded_string[in_] != '=') && is_base64(encoded_string[in_]))
    {
        char_array_4[i++] = encoded_string[in_]; in_++;
        if (i == 4)
        {
            for (i = 0; i <4; i++)
            {
                char_array_4[i] = base64_chars.find(char_array_4[i]);
            }

            char_array_3[0] = ((char_array_4[0]      ) << 2) + ((char_array_4[1] & 0x30) >> 4);
            char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
            char_array_3[2] = ((char_array_4[2] & 0x3) << 6) +   char_array_4[3];

            for (i = 0; (i < 3); i++)
            {
                ret.push_back(char_array_3[i]);
            }
            i = 0;
        }
    }

    if (i)
    {
        for (j = i; j <4; j++)
        {
            char_array_4[j] = 0;
        }

        for (j = 0; j <4; j++)
        {
            char_array_4[j] = base64_chars.find(char_array_4[j]);
        }

        char_array_3[0] = ((char_array_4[0]      ) << 2) + ((char_array_4[1] & 0x30) >> 4);
        char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
        char_array_3[2] = ((char_array_4[2] & 0x3) << 6) +   char_array_4[3];

        for (j = 0; (j < i - 1); j++)
        {
            ret.push_back(char_array_3[j]);
        }
    }

    return ret;
}

//
// parallel
//

enum task_type {
    COMPLETION_TASK,
    CANCEL_TASK
};

struct task_server {
    int id;
    int target_id;
    task_type type;
    json data;
    bool infill_mode = false;
    bool embedding_mode = false;
};

struct task_result {
    int id;
    bool stop;
    bool error;
    json result_json;
};

// TODO: can become bool if we can't find use of more states
enum slot_state
{
    IDLE,
    PROCESSING,
};

enum slot_command
{
    NONE,
    LOAD_PROMPT,
    RELEASE,
};

struct slot_params
{
    bool stream       = true;
    bool cache_prompt = false; // remember the prompt to avoid reprocessing all prompt

    uint32_t seed      = -1; // RNG seed
    int32_t  n_keep    =  0; // number of tokens to keep from initial prompt
    int32_t  n_predict = -1; // new tokens to predict

    std::vector<std::string> antiprompt;

    json input_prefix;
    json input_suffix;
};

struct slot_image
{
    int32_t id;

    bool request_encode_image = false;
    float* image_embedding = nullptr;
    int32_t image_tokens = 0;

    clip_image_u8 img_data;

    std::string prefix_prompt; // before of this image
};

// completion token output with probabilities
struct completion_token_output
{
    struct token_prob
    {
        llama_token tok;
        float prob;
    };

    std::vector<token_prob> probs;
    llama_token tok;
    std::string text_to_send;
};

static size_t common_part(const std::vector<llama_token> &a, const std::vector<llama_token> &b)
{
    size_t i;
    for (i = 0; i < a.size() && i < b.size() && a[i] == b[i]; i++)
    {
    }
    return i;
}

enum stop_type
{
    STOP_FULL,
    STOP_PARTIAL,
};

static bool ends_with(const std::string &str, const std::string &suffix)
{
    return str.size() >= suffix.size() &&
           0 == str.compare(str.size() - suffix.size(), suffix.size(), suffix);
}

static size_t find_partial_stop_string(const std::string &stop,
                                       const std::string &text)
{
    if (!text.empty() && !stop.empty())
    {
        const char text_last_char = text.back();
        for (int64_t char_index = stop.size() - 1; char_index >= 0; char_index--)
        {
            if (stop[char_index] == text_last_char)
            {
                const std::string current_partial = stop.substr(0, char_index + 1);
                if (ends_with(text, current_partial))
                {
                    return text.size() - char_index - 1;
                }
            }
        }
    }
    return std::string::npos;
}

// TODO: reuse llama_detokenize
template <class Iter>
static std::string tokens_to_str(llama_context *ctx, Iter begin, Iter end)
{
    std::string ret;
    for (; begin != end; ++begin)
    {
        ret += llama_token_to_piece(ctx, *begin);
    }
    return ret;
}

static void server_log(const char *level, const char *function, int line,
                       const char *message, const nlohmann::ordered_json &extra)
{
    nlohmann::ordered_json log
    {
        {"timestamp", time(nullptr)},
        {"level",     level},
        {"function",  function},
        {"line",      line},
        {"message",   message},
    };

    if (!extra.empty())
    {
        log.merge_patch(extra);
    }

    const std::string str = log.dump(-1, ' ', false, json::error_handler_t::replace);
    printf("%.*s\n", (int)str.size(), str.data());
    fflush(stdout);
}

// format incomplete utf-8 multibyte character for output
static std::string tokens_to_output_formatted_string(const llama_context *ctx, const llama_token token)
{
    std::string out = token == -1 ? "" : llama_token_to_piece(ctx, token);
    // if the size is 1 and first bit is 1, meaning it's a partial character
    //   (size > 1 meaning it's already a known token)
    if (out.size() == 1 && (out[0] & 0x80) == 0x80)
    {
        std::stringstream ss;
        ss << std::hex << (out[0] & 0xff);
        std::string res(ss.str());
        out = "byte: \\x" + res;
    }
    return out;
}

// convert a vector of completion_token_output to json
static json probs_vector_to_json(const llama_context *ctx, const std::vector<completion_token_output> &probs)
{
    json out = json::array();
    for (const auto &prob : probs)
    {
        json probs_for_token = json::array();
        for (const auto &p : prob.probs)
        {
            std::string tok_str = tokens_to_output_formatted_string(ctx, p.tok);
            probs_for_token.push_back(json
            {
                {"tok_str", tok_str},
                {"prob",    p.prob},
            });
        }
        std::string tok_str = tokens_to_output_formatted_string(ctx, prob.tok);
        out.push_back(json{
            {"content", tok_str},
            {"probs",   probs_for_token},
        });
    }
    return out;
}

template <typename T>
static T json_value(const json &body, const std::string &key, const T &default_value)
{
    // Fallback null to default value
    return body.contains(key) && !body.at(key).is_null()
        ? body.value(key, default_value)
        : default_value;
}

struct llama_client_slot
{
    int id;
    int task_id = -1;

    struct slot_params params;

    slot_state state = IDLE;
    slot_command command = NONE;

    // used to determine the slot that has been used the longest
    int64_t t_last_used = -1;

    // generation props
    int32_t n_ctx       = 0;  // context size per slot
    int32_t n_past      = 0;
    int32_t n_decoded   = 0;
    int32_t n_remaining = -1;
    int32_t i_batch     = -1;

    int32_t num_prompt_tokens           = 0;
    int32_t num_prompt_tokens_processed = 0;
    int32_t multibyte_pending           = 0;

    json prompt;
    std::string generated_text;
    llama_token sampled;
    std::vector<llama_token> cache_tokens;
    std::vector<completion_token_output> generated_token_probs;

    bool infill = false;
    bool embedding = false;
    bool has_next_token = true;
    bool truncated = false;
    bool stopped_eos = false;
    bool stopped_word = false;
    bool stopped_limit = false;

    std::string stopping_word;

    // sampling
    struct llama_sampling_params sparams;
    llama_sampling_context *ctx_sampling = nullptr;

    // multimodal
    std::vector<slot_image> images;

    // stats
    size_t sent_count = 0;
    size_t sent_token_probs_index = 0;

    int64_t t_start_process_prompt;
    int64_t t_start_genereration;

    double t_prompt_processing; // ms
    double t_token_generation; // ms

    void reset() {
        num_prompt_tokens      = 0;
        generated_text         = "";
        truncated              = false;
        stopped_eos            = false;
        stopped_word           = false;
        stopped_limit          = false;
        stopping_word          = "";
        multibyte_pending      = 0;
        n_past                 = 0;
        sent_count             = 0;
        sent_token_probs_index = 0;
        infill                 = false;

        generated_token_probs.clear();

        for (slot_image &img : images)
        {
            free(img.image_embedding);
            delete[] img.img_data.data;
            img.prefix_prompt = "";
        }

        images.clear();
        // llama_set_rng_seed(ctx, params.seed); in batched the seed matter???????
    }

    bool has_budget(gpt_params &global_params) {
        n_remaining = -1;
        if(params.n_predict != -1)
        {
            n_remaining = params.n_predict - n_decoded;
        }
        else if (global_params.n_predict != -1)
        {
            n_remaining = global_params.n_predict - n_decoded;
        }
        return n_remaining > 0 || n_remaining == -1; // no budget || limitless
    }

    bool available() const {
        return state == IDLE && command == NONE;
    }

    bool is_processing() const {
        return (state == IDLE && command == LOAD_PROMPT) || state == PROCESSING;
    }

    void add_token_string(const completion_token_output &token) {
        if (command == RELEASE)
        {
            return;
        }
        cache_tokens.push_back(token.tok);
        generated_token_probs.push_back(token);
    }

    void release() {
        if (state == IDLE || state == PROCESSING)
        {
            t_token_generation = (ggml_time_us() - t_start_genereration) / 1e3;
            command = RELEASE;
        }
    }

    json get_formated_timings() {
        return json
        {
            {"prompt_n",               num_prompt_tokens_processed},
            {"prompt_ms",              t_prompt_processing},
            {"prompt_per_token_ms",    t_prompt_processing / num_prompt_tokens_processed},
            {"prompt_per_second",      1e3 / t_prompt_processing * num_prompt_tokens_processed},

            {"predicted_n",            n_decoded},
            {"predicted_ms",           t_token_generation},
            {"predicted_per_token_ms", t_token_generation / n_decoded},
            {"predicted_per_second",   1e3 / t_token_generation * n_decoded},
        };
    }

    void print_timings() {
        LOG_TEE("\n");
        LOG_TEE("%s: prompt eval time = %10.2f ms / %5d tokens (%8.2f ms per token, %8.2f tokens per second)\n",
            __func__, t_prompt_processing, num_prompt_tokens_processed, t_prompt_processing / num_prompt_tokens_processed, 1e3 / t_prompt_processing * num_prompt_tokens_processed);
        LOG_TEE("%s:        eval time = %10.2f ms / %5d runs   (%8.2f ms per token, %8.2f tokens per second)\n",
            __func__, t_token_generation, n_decoded,t_token_generation / n_decoded, 1e3 / t_token_generation * n_decoded);
        LOG_TEE("%s:       total time = %10.2f ms\n", __func__, t_prompt_processing + t_token_generation);
    }
};

struct llama_server_context
{
    llama_model *model = nullptr;
    llama_context *ctx = nullptr;

    clip_ctx *clp_ctx = nullptr;

    gpt_params params;

    llama_batch batch;

    bool multimodal         = false;
    bool clean_kv_cache     = true;
    bool all_slots_are_idle = false;

    int32_t id_gen;
    int32_t n_ctx;  // total context for all clients / slots

    // system prompt
    bool system_need_update = false;

    std::string              system_prompt;
    std::vector<llama_token> system_tokens;

    std::string name_user;      // this should be the antiprompt
    std::string name_assistant;

    // slots / clients
    std::vector<llama_client_slot> slots;

    std::vector<task_server> queue_tasks;
    std::vector<task_result> queue_results;
    std::mutex mutex_tasks;
    std::mutex mutex_results;

    ~llama_server_context()
    {
        if (ctx)
        {
            llama_free(ctx);
            ctx = nullptr;
        }
        if (model)
        {
            llama_free_model(model);
            model = nullptr;
        }
    }

    bool load_model(const gpt_params &params_)
    {
        params = params_;
        if (!params.mmproj.empty()) {
            multimodal = true;
            LOG_TEE("Multi Modal Mode Enabled");
            clp_ctx = clip_model_load(params.mmproj.c_str(), /*verbosity=*/ 1);
            if(clp_ctx == nullptr) {
                LOG_ERROR("unable to load clip model", {{"model", params.mmproj}});
                return false;
            }

            if (params.n_ctx < 2048) { // request larger context for the image embedding
                params.n_ctx = 2048;
            }
        }

        std::tie(model, ctx) = llama_init_from_gpt_params(params);
        if (model == nullptr)
        {
            LOG_ERROR("unable to load model", {{"model", params.model}});
            return false;
        }

        if (multimodal) {
            const int n_embd_clip = clip_n_mmproj_embd(clp_ctx);
            const int n_embd_llm  = llama_n_embd(model);
            if (n_embd_clip != n_embd_llm) {
                LOG_TEE("%s: embedding dim of the multimodal projector (%d) is not equal to that of LLaMA (%d). Make sure that you use the correct mmproj file.\n", __func__, n_embd_clip, n_embd_llm);
                llama_free(ctx);
                llama_free_model(model);
                return false;
            }
        }

        n_ctx = llama_n_ctx(ctx);

        return true;
    }

    void initialize() {
        id_gen = 0;

        // create slots
        all_slots_are_idle = true;

        const int32_t n_ctx_slot = n_ctx / params.n_parallel;

        LOG_TEE("Available slots:\n");
        for (int i = 0; i < params.n_parallel; i++)
        {
            llama_client_slot slot;

            slot.id = i;
            slot.n_ctx = n_ctx_slot;
            slot.reset();

            LOG_TEE(" -> Slot %i - max context: %i\n", slot.id, n_ctx_slot);
            slots.push_back(slot);
        }

        batch = llama_batch_init(n_ctx, 0, params.n_parallel);

        // empty system prompt
        system_prompt = "";
        system_tokens.clear();
    }

    std::vector<llama_token> tokenize(const json & json_prompt, bool add_bos) const
    {
        // If `add_bos` is true, we only add BOS, when json_prompt is a string,
        // or the first element of the json_prompt array is a string.
        std::vector<llama_token> prompt_tokens;

        if (json_prompt.is_array())
        {
            bool first = true;
            for (const auto& p : json_prompt)
            {
                if (p.is_string())
                {
                    auto s = p.template get<std::string>();
                    std::vector<llama_token> p;
                    if (first)
                    {
                        p = ::llama_tokenize(ctx, s, add_bos);
                        first = false;
                    }
                    else
                    {
                        p = ::llama_tokenize(ctx, s, false);
                    }
                    prompt_tokens.insert(prompt_tokens.end(), p.begin(), p.end());
                }
                else
                {
                    if (first)
                    {
                        first = false;
                    }
                    prompt_tokens.push_back(p.template get<llama_token>());
                }
            }
        }
        else
        {
            auto s = json_prompt.template get<std::string>();
            prompt_tokens = ::llama_tokenize(ctx, s, add_bos);
        }

        return prompt_tokens;
    }

    llama_client_slot* get_slot(int id) {
        int64_t t_last = ggml_time_us();
        llama_client_slot *last_used = nullptr;

        for (llama_client_slot & slot : slots)
        {
            if (slot.id == id && slot.available())
            {
                return &slot;
            }

            if (slot.available() && slot.t_last_used < t_last)
            {
                last_used = &slot;
                t_last = slot.t_last_used;
            }
        }

        return last_used;
    }

    bool launch_slot_with_data(llama_client_slot* &slot, json data) {
        slot_params default_params;
        llama_sampling_params default_sparams;

        slot->params.stream           = json_value(data, "stream",            false);
        slot->params.cache_prompt     = json_value(data, "cache_prompt",      false);
        slot->params.n_predict        = json_value(data, "n_predict",         default_params.n_predict);
        slot->sparams.top_k           = json_value(data, "top_k",             default_sparams.top_k);
        slot->sparams.top_p           = json_value(data, "top_p",             default_sparams.top_p);
        slot->sparams.tfs_z           = json_value(data, "tfs_z",             default_sparams.tfs_z);
        slot->sparams.typical_p       = json_value(data, "typical_p",         default_sparams.typical_p);
        slot->sparams.temp            = json_value(data, "temperature",       default_sparams.temp);
        slot->sparams.penalty_last_n  = json_value(data, "repeat_last_n",     default_sparams.penalty_last_n);
        slot->sparams.penalty_repeat  = json_value(data, "repeat_penalty",    default_sparams.penalty_repeat);
        slot->sparams.penalty_freq    = json_value(data, "frequency_penalty", default_sparams.penalty_freq);
        slot->sparams.penalty_present = json_value(data, "presence_penalty",  default_sparams.penalty_present);
        slot->sparams.mirostat        = json_value(data, "mirostat",          default_sparams.mirostat);
        slot->sparams.mirostat_tau    = json_value(data, "mirostat_tau",      default_sparams.mirostat_tau);
        slot->sparams.mirostat_eta    = json_value(data, "mirostat_eta",      default_sparams.mirostat_eta);
        slot->sparams.penalize_nl     = json_value(data, "penalize_nl",       default_sparams.penalize_nl);
        slot->params.n_keep           = json_value(data, "n_keep",            slot->params.n_keep);
        slot->params.seed             = json_value(data, "seed",              default_params.seed);
        slot->sparams.grammar         = json_value(data, "grammar",           default_sparams.grammar);
        slot->sparams.n_probs         = json_value(data, "n_probs",           default_sparams.n_probs);

        // infill
        if (data.count("input_prefix") != 0)
        {
            slot->params.input_prefix = data["input_prefix"];
        }
        else
        {
            slot->params.input_prefix = "";
        }

        if (data.count("input_suffix") != 0)
        {
            slot->params.input_suffix = data["input_suffix"];
        }
        else
        {
            slot->params.input_suffix = "";
        }

        if (data.count("prompt") != 0)
        {
            slot->prompt = data["prompt"];
        }
        else
        {
            slot->prompt = "";
        }

        slot->sparams.logit_bias.clear();

        if (json_value(data, "ignore_eos", false))
        {
            slot->sparams.logit_bias[llama_token_eos(model)] = -INFINITY;
        }

        const auto &logit_bias = data.find("logit_bias");
        if (logit_bias != data.end() && logit_bias->is_array())
        {
            const int n_vocab = llama_n_vocab(model);
            for (const auto &el : *logit_bias)
            {
                if (el.is_array() && el.size() == 2 && el[0].is_number_integer())
                {
                    llama_token tok = el[0].get<llama_token>();
                    if (tok >= 0 && tok < n_vocab)
                    {
                        if (el[1].is_number())
                        {
                            slot->sparams.logit_bias[tok] = el[1].get<float>();
                        }
                        else if (el[1].is_boolean() && !el[1].get<bool>())
                        {
                            slot->sparams.logit_bias[tok] = -INFINITY;
                        }
                    }
                }
            }
        }

        slot->params.antiprompt.clear();

        const auto &stop = data.find("stop");
        if (stop != data.end() && stop->is_array())
        {
            for (const auto &word : *stop)
            {
                if (!word.empty())
                {
                    slot->params.antiprompt.push_back(word);
                }
            }
        }

        if (multimodal)
        {
            const auto &images_data = data.find("image_data");
            if (images_data != data.end() && images_data->is_array())
            {
                for (const auto &img : *images_data)
                {
                    std::string data_b64 = img["data"].get<std::string>();
                    slot_image img_sl;
                    img_sl.id = img.count("id") != 0 ? img["id"].get<int>() : slot->images.size();
                    int width, height, channels;
                    std::vector<uint8_t> image_buffer = base64_decode(data_b64);
                    data_b64.clear();
                    auto data = stbi_load_from_memory(image_buffer.data(), image_buffer.size(), &width, &height, &channels, 3);
                    if (!data) {
                        LOG_TEE("slot %i - failed to load image [id: %i]\n", slot->id, img_sl.id);
                        return false;
                    }
                    LOG_TEE("slot %i - image loaded [id: %i] resolution (%i x %i)\n", slot->id, img_sl.id, width, height);
                    img_sl.img_data.nx = width;
                    img_sl.img_data.ny = height;
                    img_sl.img_data.size = width * height * 3;
                    img_sl.img_data.data = new uint8_t[width * height * 3]();
                    memcpy(img_sl.img_data.data, data, width * height * 3);
                    stbi_image_free(data);
                    img_sl.request_encode_image = true;
                    slot->images.push_back(img_sl);
                }
                // process prompt
                // example: system prompt [img-102] user [img-103] describe [img-134] -> [{id: 102, prefix: 'system prompt '}, {id: 103, prefix: ' user '}, {id: 134, prefix: ' describe '}]}
                if (slot->images.size() > 0 && !slot->prompt.is_array())
                {
                    std::string prompt = slot->prompt.get<std::string>();
                    size_t pos = 0, begin_prefix = 0;
                    std::string pattern = "[img-";
                    while ((pos = prompt.find(pattern, pos)) != std::string::npos) {
                        size_t end_prefix = pos;
                        pos += pattern.length();
                        size_t end_pos = prompt.find("]", pos);
                        if (end_pos != std::string::npos)
                        {
                            std::string image_id = prompt.substr(pos, end_pos - pos);
                            try
                            {
                                int img_id = std::stoi(image_id);
                                bool found = false;
                                for (slot_image &img : slot->images)
                                {
                                    if (img.id == img_id) {
                                        found = true;
                                        img.prefix_prompt = prompt.substr(begin_prefix, end_prefix - begin_prefix);
                                        begin_prefix = end_pos + 1;
                                        break;
                                    }
                                }
                                if (!found) {
                                    LOG_TEE("ERROR: Image with id: %i, not found.\n", img_id);
                                    slot->images.clear();
                                    return false;
                                }
                            } catch (const std::invalid_argument& e) {
                                LOG_TEE("Invalid image number id in prompt\n");
                                slot->images.clear();
                                return false;
                            }
                        }
                    }
                    slot->prompt = "";
                    slot->params.input_suffix = prompt.substr(begin_prefix);
                    slot->params.cache_prompt = false; // multimodal doesn't support cache prompt
                }
            }
        }

        if (slot->ctx_sampling != nullptr)
        {
            llama_sampling_free(slot->ctx_sampling);
        }
        slot->ctx_sampling = llama_sampling_init(slot->sparams);
        slot->command = LOAD_PROMPT;

        all_slots_are_idle = false;

        LOG_TEE("slot %i is processing [task id: %i]\n", slot->id, slot->task_id);

        return true;
    }

    void kv_cache_clear() {
        // clear the entire KV cache
        llama_kv_cache_clear(ctx);
        clean_kv_cache = false;
    }

    void update_system_prompt() {
        system_tokens = ::llama_tokenize(ctx, system_prompt, true);

        llama_batch_clear(batch);

        kv_cache_clear();

        for (int i = 0; i < (int) system_tokens.size(); ++i)
        {
            llama_batch_add(batch, system_tokens[i], i, { 0 }, false);
        }

        if (llama_decode(ctx, batch) != 0)
        {
            LOG_TEE("%s: llama_decode() failed\n", __func__);
            return;
        }

        // assign the system KV cache to all parallel sequences
        for (int32_t i = 1; i < params.n_parallel; ++i)
        {
            llama_kv_cache_seq_cp(ctx, 0, i, 0, system_tokens.size());
        }

        LOG_TEE("system prompt updated\n");
        system_need_update = false;
    }

    void notify_system_prompt_changed() {
        // release all slots
        for (llama_client_slot &slot : slots)
        {
            slot.release();
        }

        system_need_update = true;
    }

    void process_system_prompt_data(const json &sys_props) {
        system_prompt  = sys_props.value("prompt", "");
        name_user      = sys_props.value("anti_prompt", "");
        name_assistant = sys_props.value("assistant_name", "");

        if (slots.size() > 0)
        {
            notify_system_prompt_changed();
        }
    }

    static size_t find_stopping_strings(const std::string &text, const size_t last_token_size,
                                        const stop_type type, llama_client_slot &slot)
    {
        size_t stop_pos = std::string::npos;

        for (const std::string &word : slot.params.antiprompt)
        {
            size_t pos;
            if (type == STOP_FULL)
            {
                const size_t tmp = word.size() + last_token_size;
                const size_t from_pos = text.size() > tmp ? text.size() - tmp : 0;
                pos = text.find(word, from_pos);
            }
            else
            {
                pos = find_partial_stop_string(word, text);
            }
            if (pos != std::string::npos &&
                (stop_pos == std::string::npos || pos < stop_pos))
            {
                if (type == STOP_FULL)
                {
                    slot.stopped_word = true;
                    slot.stopping_word = word;
                    slot.has_next_token = false;
                }
                stop_pos = pos;
            }
        }

        return stop_pos;
    }

    bool process_token(completion_token_output &result, llama_client_slot &slot) {
        // remember which tokens were sampled - used for repetition penalties during sampling
        const std::string token_str = llama_token_to_piece(ctx, result.tok);
        slot.sampled = result.tok;

        // search stop word and delete it
        slot.generated_text += token_str;
        slot.has_next_token = true;

        if (slot.multibyte_pending > 0)
        {
            slot.multibyte_pending -= token_str.size();
        }
        else if (token_str.size() == 1)
        {
            const char c = token_str[0];
            // 2-byte characters: 110xxxxx 10xxxxxx
            if ((c & 0xE0) == 0xC0)
            {
                slot.multibyte_pending = 1;
                // 3-byte characters: 1110xxxx 10xxxxxx 10xxxxxx
            }
            else if ((c & 0xF0) == 0xE0)
            {
                slot.multibyte_pending = 2;
                // 4-byte characters: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
            }
            else if ((c & 0xF8) == 0xF0)
            {
                slot.multibyte_pending = 3;
            }
            else
            {
                slot.multibyte_pending = 0;
            }
        }

        if (slot.multibyte_pending == 0)
        {
            size_t pos = std::min(slot.sent_count, slot.generated_text.size());
            const std::string str_test = slot.generated_text.substr(pos);
            bool is_stop_full = false;
            size_t stop_pos = find_stopping_strings(str_test, token_str.size(), STOP_FULL, slot);
            if (stop_pos != std::string::npos)
            {
                is_stop_full = true;
                slot.generated_text.erase(
                    slot.generated_text.begin() + pos + stop_pos,
                    slot.generated_text.end());
                pos = std::min(slot.sent_count, slot.generated_text.size());
            }
            else
            {
                is_stop_full = false;
                stop_pos = find_stopping_strings(str_test, token_str.size(), STOP_PARTIAL, slot);
            }

            // check if there is any token to predict
            if (stop_pos == std::string::npos || (!slot.has_next_token && !is_stop_full && stop_pos > 0))
            {
                // no send the stop word in the response
                result.text_to_send = slot.generated_text.substr(pos, std::string::npos);
                slot.sent_count += result.text_to_send.size();
                // add the token to slot queue and cache
            }
            slot.add_token_string(result);
            if (slot.params.stream)
            {
                send_partial_response(slot, result);
            }
        }

        if (slot.multibyte_pending > 0 && !slot.has_next_token)
        {
            slot.has_next_token = true;
        }

        // check the limits
        if (slot.n_decoded > 2 && slot.has_next_token && !slot.has_budget(params))
        {
            slot.stopped_limit = true;
            slot.has_next_token = false;
        }

        if (!slot.cache_tokens.empty() && result.tok == llama_token_eos(model))
        {
            slot.stopped_eos = true;
            slot.has_next_token = false;
            LOG_VERBOSE("eos token found", {});
        }

        LOG_VERBOSE("next token", {
                                      {"token", result.tok},
                                      {"token_text", tokens_to_output_formatted_string(ctx, result.tok)},
                                      {"has_next_token", slot.has_next_token},
                                      {"n_remain", slot.n_remaining},
                                      {"num_tokens_predicted", slot.n_decoded},
                                      {"stopped_eos", slot.stopped_eos},
                                      {"stopped_word", slot.stopped_word},
                                      {"stopped_limit", slot.stopped_limit},
                                      {"stopping_word", slot.stopping_word},
                                  });

        return slot.has_next_token; // continue
    }

    bool process_images(llama_client_slot &slot) const
    {
        for (slot_image &img : slot.images)
        {
            if (!img.request_encode_image)
            {
                continue;
            }
            clip_image_f32 img_res;
            if (!clip_image_preprocess(clp_ctx, &img.img_data, &img_res, /*pad2square =*/ true))
            {
                LOG_TEE("Error processing the given image");
                clip_free(clp_ctx);
                return false;
            }
            img.image_tokens = clip_n_patches(clp_ctx);
            img.image_embedding = (float *)malloc(clip_embd_nbytes(clp_ctx));
            if (!img.image_embedding)
            {
                LOG_TEE("Unable to allocate memory for image embeddings\n");
                clip_free(clp_ctx);
                return false;
            }
            LOG_TEE("slot %i - encoding image [id: %i]\n", slot.id, img.id);
            if (!clip_image_encode(clp_ctx, params.n_threads, &img_res, img.image_embedding))
            {
                LOG_TEE("Unable to encode image\n");
                return false;
            }
            img.request_encode_image = false;
        }

        return slot.images.size() > 0;
    }

    void send_error(int id, std::string error)
    {
        std::lock_guard<std::mutex> lock(mutex_results);
        task_result res;
        res.id = id;
        res.error = true;
        res.result_json = { { "content", error } };
        queue_results.push_back(res);
    }

    json get_model_props()
    {
        return get_formated_generation(slots[0]);
    }

    json get_formated_generation(llama_client_slot &slot)
    {
        const auto eos_bias = slot.sparams.logit_bias.find(llama_token_eos(model));
        const bool ignore_eos = eos_bias != slot.sparams.logit_bias.end() &&
                                eos_bias->second < 0.0f && std::isinf(eos_bias->second);
        return json {
            {"n_ctx",             slot.n_ctx},
            {"model",             params.model_alias},
            {"seed",              slot.params.seed},
            {"temp",              slot.sparams.temp},
            {"top_k",             slot.sparams.top_k},
            {"top_p",             slot.sparams.top_p},
            {"tfs_z",             slot.sparams.tfs_z},
            {"typical_p",         slot.sparams.typical_p},
            {"repeat_last_n",     slot.sparams.penalty_last_n},
            {"repeat_penalty",    slot.sparams.penalty_repeat},
            {"presence_penalty",  slot.sparams.penalty_present},
            {"frequency_penalty", slot.sparams.penalty_freq},
            {"mirostat",          slot.sparams.mirostat},
            {"mirostat_tau",      slot.sparams.mirostat_tau},
            {"mirostat_eta",      slot.sparams.mirostat_eta},
            {"penalize_nl",       slot.sparams.penalize_nl},
            {"stop",              slot.params.antiprompt},
            {"n_predict",         slot.params.n_predict},
            {"n_keep",            params.n_keep},
            {"ignore_eos",        ignore_eos},
            {"stream",            slot.params.stream},
            {"logit_bias",        slot.sparams.logit_bias},
            {"n_probs",           slot.sparams.n_probs},
            {"grammar",           slot.sparams.grammar},
        };
    }

    void send_partial_response(llama_client_slot &slot, completion_token_output tkn)
    {
        std::lock_guard<std::mutex> lock(mutex_results);
        task_result res;
        res.id = slot.task_id;
        res.error = false;
        res.stop = false;

        res.result_json = json
        {
            {"content",    tkn.text_to_send},
            {"stop",       false},
            {"slot_id",    slot.id},
            {"multimodal", multimodal}
        };

        if (slot.sparams.n_probs > 0)
        {
            std::vector<completion_token_output> probs_output = {};
            const std::vector<llama_token> to_send_toks = llama_tokenize(ctx, tkn.text_to_send, false);
            size_t probs_pos = std::min(slot.sent_token_probs_index, slot.generated_token_probs.size());
            size_t probs_stop_pos = std::min(slot.sent_token_probs_index + to_send_toks.size(), slot.generated_token_probs.size());
            if (probs_pos < probs_stop_pos)
            {
                probs_output = std::vector<completion_token_output>(slot.generated_token_probs.begin() + probs_pos, slot.generated_token_probs.begin() + probs_stop_pos);
            }
            slot.sent_token_probs_index = probs_stop_pos;
            res.result_json["completion_probabilities"] = probs_vector_to_json(ctx, probs_output);
        }

        queue_results.push_back(res);
    }

    void send_final_response(llama_client_slot &slot)
    {
        std::lock_guard<std::mutex> lock(mutex_results);
        task_result res;
        res.id = slot.task_id;
        res.error = false;
        res.stop = true;

        res.result_json = json
        {
            {"content",             !slot.params.stream ? slot.generated_text : ""},
            {"slot_id",             slot.id},
            {"stop",                true},
            {"model",               params.model_alias},
            {"tokens_predicted",    slot.n_decoded},
            {"tokens_evaluated",    slot.num_prompt_tokens},
            {"generation_settings", get_formated_generation(slot)},
            {"prompt",              slot.prompt},
            {"truncated",           slot.truncated},
            {"stopped_eos",         slot.stopped_eos},
            {"stopped_word",        slot.stopped_word},
            {"stopped_limit",       slot.stopped_limit},
            {"stopping_word",       slot.stopping_word},
            {"tokens_cached",       slot.n_past},
            {"timings",             slot.get_formated_timings()}
        };

        if (slot.sparams.n_probs > 0)
        {
            std::vector<completion_token_output> probs = {};
            if (!slot.params.stream && slot.stopped_word)
            {
                const std::vector<llama_token> stop_word_toks = llama_tokenize(ctx, slot.stopping_word, false);
                probs = std::vector<completion_token_output>(slot.generated_token_probs.begin(), slot.generated_token_probs.end() - stop_word_toks.size());
            }
            else
            {
                probs = std::vector<completion_token_output>(
                                    slot.generated_token_probs.begin(),
                                    slot.generated_token_probs.begin() + slot.sent_token_probs_index);
            }
            res.result_json["completion_probabilities"] = probs_vector_to_json(ctx, probs);
        }

        queue_results.push_back(res);
    }

    void send_embedding(llama_client_slot &slot)
    {
        std::lock_guard<std::mutex> lock(mutex_results);
        task_result res;
        res.id = slot.task_id;
        res.error = false;
        res.stop = true;

        const int n_embd = llama_n_embd(model);
        if (!params.embedding)
        {
            LOG_WARNING("embedding disabled", {
                                                  {"params.embedding", params.embedding},
                                              });
            res.result_json = json
            {
                {"embedding", std::vector<float>(n_embd, 0.0f)},
            };
        }
        else
        {
            const float *data = llama_get_embeddings(ctx);
            std::vector<float> embedding(data, data + n_embd);
            res.result_json = json
            {
                {"embedding", embedding },
            };
        }
        queue_results.push_back(res);
    }

    int request_completion(json data, bool infill, bool embedding)
    {
        std::lock_guard<std::mutex> lock(mutex_tasks);
        task_server task;
        task.id = id_gen++;
        task.data = data;
        task.infill_mode = infill;
        task.embedding_mode = embedding;
        task.type = COMPLETION_TASK;
        queue_tasks.push_back(task);
        return task.id;
    }

    task_result next_result(int task_id)
    {
        while (true)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(5));
            std::lock_guard<std::mutex> lock(mutex_results);

            if (queue_results.empty())
            {
                continue;
            }

            for (int i = 0; i < (int) queue_results.size(); i++)
            {
                if (queue_results[i].id == task_id)
                {
                    task_result res = queue_results[i];
                    queue_results.erase(queue_results.begin() + i);
                    return res;
                }
            }
        }

        // never reached
        //return task_result{-1, false, false, {}};
    }

    // for multiple images processing
    bool ingest_images(llama_client_slot &slot, int n_batch)
    {
        int image_idx = 0;

        while (image_idx < (int) slot.images.size())
        {
            slot_image &img = slot.images[image_idx];

            // process prefix prompt
            for (int32_t i = 0; i < (int32_t) batch.n_tokens; i += n_batch)
            {
                const int32_t n_tokens = std::min(n_batch, (int32_t) (batch.n_tokens - i));
                llama_batch batch_view = {
                    n_tokens,
                    batch.token    + i,
                    nullptr,
                    batch.pos      + i,
                    batch.n_seq_id + i,
                    batch.seq_id   + i,
                    batch.logits   + i,
                    0, 0, 0, // unused
                };
                if (llama_decode(ctx, batch_view))
                {
                    LOG_TEE("%s : failed to eval\n", __func__);
                    return false;
                }
            }

            // process image with llm
            for (int i = 0; i < img.image_tokens; i += n_batch)
            {
                int n_eval = img.image_tokens - i;
                if (n_eval > n_batch)
                {
                    n_eval = n_batch;
                }

                const int n_embd = llama_n_embd(model);
                llama_batch batch_img = { n_eval, nullptr, (img.image_embedding + i * n_embd), nullptr, nullptr, nullptr, nullptr, slot.n_past, 1, 0, };
                if (llama_decode(ctx, batch_img))
                {
                    LOG_TEE("%s : failed to eval image\n", __func__);
                    return false;
                }
                slot.n_past += n_eval;
            }
            image_idx++;

            llama_batch_clear(batch);

            // append prefix of next image
            const auto json_prompt = (image_idx >= (int) slot.images.size()) ?
                slot.params.input_suffix : // no more images, then process suffix prompt
                (json)(slot.images[image_idx].prefix_prompt);

            std::vector<llama_token> append_tokens = tokenize(json_prompt, false); // has next image
            for (int i = 0; i < (int) append_tokens.size(); ++i)
            {
                llama_batch_add(batch, append_tokens[i], slot.n_past, { slot.id }, true);
                slot.n_past += 1;
            }
        }

        return true;
    }

    void request_cancel(int task_id)
    {
        std::lock_guard<std::mutex> lock(mutex_tasks);
        task_server task;
        task.id = id_gen++;
        task.type = CANCEL_TASK;
        task.target_id = task_id;
        queue_tasks.push_back(task);
    }

    void process_tasks()
    {
        std::lock_guard<std::mutex> lock(mutex_tasks);
        while (!queue_tasks.empty())
        {
            task_server task = queue_tasks.front();
            queue_tasks.erase(queue_tasks.begin());
            switch (task.type)
            {
                case COMPLETION_TASK: {
                    llama_client_slot *slot = get_slot(json_value(task.data, "slot_id", -1));
                    if (slot == nullptr)
                    {
                        LOG_TEE("slot unavailable\n");
                        // send error result
                        send_error(task.id, "slot unavailable");
                        return;
                    }

                    if (task.data.contains("system_prompt"))
                    {
                        process_system_prompt_data(task.data["system_prompt"]);
                    }

                    slot->reset();

                    slot->infill = task.infill_mode;
                    slot->embedding = task.embedding_mode;
                    slot->task_id = task.id;

                    if (!launch_slot_with_data(slot, task.data))
                    {
                        // send error result
                        send_error(task.id, "internal_error");
                        break;
                    }
                } break;
                case CANCEL_TASK: { // release slot linked with the task id
                    for (auto & slot : slots)
                    {
                        if (slot.task_id == task.target_id)
                        {
                            slot.release();
                            break;
                        }
                    }
                } break;
            }
        }
    }

    bool update_slots() {
        // attend tasks
        process_tasks();

        // update the system prompt wait until all slots are idle state
        if (system_need_update && all_slots_are_idle)
        {
            LOG_TEE("updating system prompt\n");
            update_system_prompt();
        }

        llama_batch_clear(batch);

        if (all_slots_are_idle)
        {
            if (system_prompt.empty() && clean_kv_cache)
            {
                LOG_TEE("all slots are idle and system prompt is empty, clear the KV cache\n");
                kv_cache_clear();
            }
            // avoid 100% usage of cpu all time
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        for (llama_client_slot &slot : slots)
        {
            if (slot.is_processing() && slot.cache_tokens.size() >= (size_t) slot.n_ctx)
            {
                // Shift context
                const int n_left    = slot.n_past - slot.params.n_keep - 1;
                const int n_discard = n_left / 2;

                LOG_TEE("slot %d: context shift - n_keep = %d, n_left = %d, n_discard = %d\n", slot.id, slot.params.n_keep, n_left, n_discard);
                llama_kv_cache_seq_rm   (ctx, slot.id, slot.params.n_keep + 1            , slot.params.n_keep + n_discard + 1);
                llama_kv_cache_seq_shift(ctx, slot.id, slot.params.n_keep + 1 + n_discard, slot.n_past, -n_discard);

                for (size_t i = slot.params.n_keep + 1 + n_discard; i < slot.cache_tokens.size(); i++)
                {
                    slot.cache_tokens[i - n_discard] = slot.cache_tokens[i];
                }

                slot.cache_tokens.resize(slot.cache_tokens.size() - n_discard);

                slot.n_past -= n_discard;

                slot.truncated = true;

                LOG_VERBOSE("context shift", {
                                                {"n_ctx",  n_ctx},
                                                {"n_keep", params.n_keep},
                                                {"n_left", n_left},
                                            });
            }
        }

        // decode any currently ongoing sequences
        for (auto & slot : slots)
        {
            // release the slot
            if (slot.command == RELEASE)
            {
                slot.state = IDLE;
                slot.command = NONE;
                slot.t_last_used = ggml_time_us();

                LOG_TEE("slot %d released (%d tokens in cache)\n", slot.id, (int) slot.cache_tokens.size());

                continue;
            }

            if (slot.state == IDLE)
            {
                continue;
            }

            slot.i_batch = batch.n_tokens;

            llama_batch_add(batch, slot.sampled, system_tokens.size() + slot.n_past, { slot.id }, true);

            slot.n_decoded += 1;
            slot.n_past += 1;
        }

        // process in chunks of params.n_batch
        int32_t n_batch = params.n_batch;

        // assign workload to the slots
        if (params.cont_batching || batch.n_tokens == 0)
        {
            for (auto & slot : slots)
            {
                const bool has_prompt = slot.prompt.is_array() || (slot.prompt.is_string() && !slot.prompt.get<std::string>().empty()) || !slot.images.empty();

                // empty prompt passed -> release the slot and send empty response
                if (slot.state == IDLE && slot.command == LOAD_PROMPT && !has_prompt)
                {
                    slot.release();
                    slot.print_timings();
                    send_final_response(slot);
                    continue;
                }

                // need process the prompt
                if (slot.state == IDLE && slot.command == LOAD_PROMPT)
                {
                    slot.state = PROCESSING;
                    slot.command = NONE;
                    std::vector<llama_token> prompt_tokens;
                    slot.t_start_process_prompt = ggml_time_us();
                    slot.t_start_genereration = 0;

                    if (slot.infill)
                    {
                        bool suff_rm_leading_spc = true;
                        if (params.input_suffix.find_first_of(' ') == 0 && params.input_suffix.size() > 1)
                        {
                            params.input_suffix.erase(0, 1);
                            suff_rm_leading_spc = false;
                        }
                        auto prefix_tokens = tokenize(slot.params.input_prefix, false);
                        auto suffix_tokens = tokenize(slot.params.input_suffix, false);

                        const int space_token = 29871; // TODO: this should not be hardcoded
                        if (suff_rm_leading_spc && !suffix_tokens.empty() && suffix_tokens[0] == space_token) {
                            suffix_tokens.erase(suffix_tokens.begin());
                        }

                        prefix_tokens.insert(prefix_tokens.begin(), llama_token_prefix(model));
                        prefix_tokens.insert(prefix_tokens.begin(), llama_token_bos(model)); // always add BOS
                        prefix_tokens.insert(prefix_tokens.end(), llama_token_suffix(model));
                        prefix_tokens.insert(prefix_tokens.end(), suffix_tokens.begin(), suffix_tokens.end());
                        prefix_tokens.push_back(llama_token_middle(model));
                        prompt_tokens = prefix_tokens;
                    }
                    else
                    {
                        prompt_tokens = tokenize(slot.prompt, system_prompt.empty());  // add BOS if there isn't system prompt
                    }

                    slot.num_prompt_tokens = prompt_tokens.size();

                    if (!slot.params.cache_prompt)
                    {
                        llama_sampling_reset(slot.ctx_sampling);

                        slot.n_past = 0;
                        slot.num_prompt_tokens_processed = slot.num_prompt_tokens;
                    }
                    else
                    {
                        if (slot.params.n_keep < 0)
                        {
                            slot.params.n_keep = slot.num_prompt_tokens;
                        }
                        slot.params.n_keep = std::min(slot.n_ctx - 4, slot.params.n_keep);

                        // if input prompt is too big, truncate it
                        if (slot.num_prompt_tokens >= slot.n_ctx)
                        {
                            const int n_left = slot.n_ctx - slot.params.n_keep;
                            const int n_block_size = n_left / 2;
                            const int erased_blocks = (slot.num_prompt_tokens - slot.params.n_keep - n_block_size) / n_block_size;

                            std::vector<llama_token> new_tokens(prompt_tokens.begin(), prompt_tokens.begin() + slot.params.n_keep);
                            new_tokens.insert(new_tokens.end(), prompt_tokens.begin() + slot.params.n_keep + erased_blocks * n_block_size, prompt_tokens.end());

                            LOG_VERBOSE("input truncated", {
                                                            {"n_ctx",  slot.n_ctx},
                                                            {"n_keep", slot.params.n_keep},
                                                            {"n_left", n_left},
                                                            {"new_tokens", tokens_to_str(ctx, new_tokens.cbegin(), new_tokens.cend())},
                                                        });
                            slot.truncated = true;
                            prompt_tokens = new_tokens;

                            slot.num_prompt_tokens = prompt_tokens.size();
                            GGML_ASSERT(slot.num_prompt_tokens < slot.n_ctx);
                        }

                        // push the prompt into the sampling context (do not apply grammar)
                        for (auto &token : prompt_tokens)
                        {
                            llama_sampling_accept(slot.ctx_sampling, ctx, token, false);
                        }

                        slot.n_past = common_part(slot.cache_tokens, prompt_tokens);
                        slot.num_prompt_tokens_processed = slot.num_prompt_tokens - slot.n_past;

                        LOG_TEE("slot %d : in cache: %i tokens | to process: %i tokens\n", slot.id, slot.n_past, slot.num_prompt_tokens_processed);
                    }

                    LOG_TEE("slot %d : kv cache rm - [%d, end)\n", slot.id, (int) system_tokens.size() + slot.n_past);

                    llama_kv_cache_seq_rm(ctx, slot.id, system_tokens.size() + slot.n_past, -1);

                    slot.cache_tokens = prompt_tokens;

                    if (slot.n_past == slot.num_prompt_tokens)
                    {
                        // we have to evaluate at least 1 token to generate logits.
                        LOG_TEE("slot %d : we have to evaluate at least 1 token to generate logits\n", slot.id);
                        slot.n_past--;
                    }

                    LOG_VERBOSE("prompt ingested", {
                                                    {"n_past", slot.n_past},
                                                    {"cached", tokens_to_str(ctx, slot.cache_tokens.cbegin(), slot.cache_tokens.cbegin() + slot.n_past)},
                                                    {"to_eval", tokens_to_str(ctx, slot.cache_tokens.cbegin() + slot.n_past, slot.cache_tokens.cend())},
                                                });

                    const bool has_images = process_images(slot);

                    // process the prefix of first image
                    std::vector<llama_token> prefix_tokens = has_images ? tokenize(slot.images[0].prefix_prompt, true) : prompt_tokens;
                    for (; slot.n_past < (int) prefix_tokens.size(); ++slot.n_past)
                    {
                       llama_batch_add(batch, prefix_tokens[slot.n_past], system_tokens.size() + slot.n_past, { slot.id }, false);
                    }

                    if (has_images && !ingest_images(slot, n_batch))
                    {
                        LOG_TEE("failed processing images\n");
                        return false;
                    }

                    // extract the logits only for the last token
                    if (batch.n_tokens > 0)
                    {
                        batch.logits[batch.n_tokens - 1] = true;
                    }

                    slot.n_decoded = 0;
                    slot.i_batch   = batch.n_tokens - 1;
                }
            }
        }

        if (batch.n_tokens == 0)
        {
            all_slots_are_idle = true;
            return true;
        }

        for (int32_t i = 0; i < (int32_t) batch.n_tokens; i += n_batch)
        {
            const int32_t n_tokens = std::min(n_batch, (int32_t) (batch.n_tokens - i));
            llama_batch batch_view =
            {
                n_tokens,
                batch.token    + i,
                nullptr,
                batch.pos      + i,
                batch.n_seq_id + i,
                batch.seq_id   + i,
                batch.logits   + i,
                0, 0, 0, // unused
            };

            const int ret = llama_decode(ctx, batch_view);
            if (ret != 0)
            {
                if (n_batch == 1 || ret < 0)
                {
                    // if you get here, it means the KV cache is full - try increasing it via the context size
                    LOG_TEE("%s : failed to decode the batch, n_batch = %d, ret = %d\n", __func__, n_batch, ret);
                    return false;
                }

                LOG_TEE("%s : failed to find free space in the KV cache, retrying with smaller n_batch = %d\n", __func__, n_batch / 2);

                // retry with half the batch size to try to find a free slot in the KV cache
                n_batch /= 2;
                i -= n_batch;
                continue;
            }

            for (auto & slot : slots)
            {
                if (slot.i_batch < (int) i || slot.i_batch >= (int) (i + n_tokens))
                {
                    continue;
                }

                // prompt evaluated for embedding
                if (slot.embedding)
                {
                    send_embedding(slot);
                    slot.release();
                    slot.i_batch = -1;
                    return true;
                }

                completion_token_output result;
                const llama_token id = llama_sampling_sample(slot.ctx_sampling, ctx, NULL, slot.i_batch - i);

                llama_sampling_accept(slot.ctx_sampling, ctx, id, true);

                if (slot.n_decoded == 1)
                {
                    slot.t_start_genereration = ggml_time_us();
                    slot.t_prompt_processing = (slot.t_start_genereration - slot.t_start_process_prompt) / 1e3;
                }

                llama_token_data_array cur_p = { slot.ctx_sampling->cur.data(), slot.ctx_sampling->cur.size(), false };
                result.tok = id;

                const int32_t n_probs = slot.sparams.n_probs;
                if (slot.sparams.temp <= 0 && n_probs > 0)
                {
                    // for llama_sample_token_greedy we need to sort candidates
                    llama_sample_softmax(ctx, &cur_p);
                }

                for (size_t i = 0; i < std::min(cur_p.size, (size_t)n_probs); ++i)
                {
                    result.probs.push_back({cur_p.data[i].id, cur_p.data[i].p});
                }

                if (!process_token(result, slot))
                {
                    slot.release();
                    slot.print_timings();
                    send_final_response(slot);
                }

                slot.i_batch = -1;
            }
        }
        return true;
    }
};



static json format_partial_response(
    llama_server_context &llama, llama_client_slot *slot, const std::string &content, const std::vector<completion_token_output> &probs
) {
    json res = json
    {
        {"content",    content },
        {"stop",       false},
        {"slot_id",    slot->id },
        {"multimodal", llama.multimodal }
    };

    if (slot->sparams.n_probs > 0)
    {
        res["completion_probabilities"] = probs_vector_to_json(llama.ctx, probs);
    }

    return res;
}

static json format_tokenizer_response(const std::vector<llama_token> &tokens)
{
    return json{
        {"tokens", tokens}};
}

static json format_detokenized_response(std::string content)
{
    return json{
        {"content", content}};
}



struct token_translator
{
    llama_context * ctx;
    std::string operator()(llama_token tok)                    const { return llama_token_to_piece(ctx, tok); }
    std::string operator()(const completion_token_output &cto) const { return (*this)(cto.tok); }
};

static void append_to_generated_text_from_generated_token_probs(llama_server_context &llama, llama_client_slot *slot)
{
    auto & gtps = slot->generated_token_probs;
    auto translator = token_translator{llama.ctx};
    auto add_strlen = [=](size_t sum, const completion_token_output & cto) { return sum + translator(cto).size(); };
    const size_t len = std::accumulate(gtps.begin(), gtps.end(), size_t(0), add_strlen);
    if (slot->generated_text.capacity() < slot->generated_text.size() + len)
    {
        slot->generated_text.reserve(slot->generated_text.size() + len);
    }
    for (const completion_token_output & cto : gtps)
    {
        slot->generated_text += translator(cto);
    }
}

/////////////////////////////////
////////////////////////////////
//////// LOCALAI code starts below here
/////////////////////////////////
////////////////////////////////

bool loaded_model; // TODO: add a mutex for this, but happens only once loading the model

// The class has a llama instance that is shared across all RPCs
llama_server_context llama;

static void start_llama_server() {
    // Wait for model to be loaded first
    while (!loaded_model) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    bool running = true;
    while (running)
    {
        running = llama.update_slots();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

json parse_options(bool streaming, const backend::PredictOptions* predict, llama_server_context &llama)
{
    
    // This is for example a slot data from the json data
    //     slot->params.stream           = json_value(data, "stream",            false);
    //     slot->params.cache_prompt     = json_value(data, "cache_prompt",      false);
    //     slot->params.n_predict        = json_value(data, "n_predict",         default_params.n_predict);
    //     slot->sparams.top_k           = json_value(data, "top_k",             default_sparams.top_k);
    //     slot->sparams.top_p           = json_value(data, "top_p",             default_sparams.top_p);
    //     slot->sparams.tfs_z           = json_value(data, "tfs_z",             default_sparams.tfs_z);
    //     slot->sparams.typical_p       = json_value(data, "typical_p",         default_sparams.typical_p);
    //     slot->sparams.temp            = json_value(data, "temperature",       default_sparams.temp);
    //     slot->sparams.penalty_last_n  = json_value(data, "repeat_last_n",     default_sparams.penalty_last_n);
    //     slot->sparams.penalty_repeat  = json_value(data, "repeat_penalty",    default_sparams.penalty_repeat);
    //     slot->sparams.penalty_freq    = json_value(data, "frequency_penalty", default_sparams.penalty_freq);
    //     slot->sparams.penalty_present = json_value(data, "presence_penalty",  default_sparams.penalty_present);
    //     slot->sparams.mirostat        = json_value(data, "mirostat",          default_sparams.mirostat);
    //     slot->sparams.mirostat_tau    = json_value(data, "mirostat_tau",      default_sparams.mirostat_tau);
    //     slot->sparams.mirostat_eta    = json_value(data, "mirostat_eta",      default_sparams.mirostat_eta);
    //     slot->sparams.penalize_nl     = json_value(data, "penalize_nl",       default_sparams.penalize_nl);
    //     slot->params.n_keep           = json_value(data, "n_keep",            slot->params.n_keep);
    //     slot->params.seed             = json_value(data, "seed",              default_params.seed);
    //     slot->sparams.grammar         = json_value(data, "grammar",           default_sparams.grammar);
    //     slot->sparams.n_probs         = json_value(data, "n_probs",           default_sparams.n_probs);

    // Create now a json data from the prediction options instead
    //
    json data;
    data["stream"] = streaming;
    data["cache_prompt"] = predict->promptcacheall();
    data["n_predict"] = predict->tokens() == 0 ? -1 : predict->tokens();
    data["top_k"] = predict->topk();
    data["top_p"] = predict->topp();
    data["tfs_z"] = predict->tailfreesamplingz();
    data["typical_p"] = predict->typicalp();
    data["temperature"] = predict->temperature();
    data["repeat_last_n"] = predict->repeat();
    data["repeat_penalty"] = predict->penalty();
    data["frequency_penalty"] = predict->frequencypenalty();
    data["presence_penalty"] = predict->presencepenalty();
    data["mirostat"] = predict->mirostat();
    data["mirostat_tau"] = predict->mirostattau();
    data["mirostat_eta"] = predict->mirostateta();
    data["penalize_nl"] = predict->penalizenl();
    data["n_keep"] = predict->nkeep();
    data["seed"] = predict->seed();
    data["grammar"] = predict->grammar();
    data["prompt"] = predict->prompt();
    data["ignore_eos"] = predict->ignoreeos();

    // for each image in the request, add the image data
    //
    for (int i = 0; i < predict->images_size(); i++) {
        data["image_data"].push_back(json
            {
                {"id", i},
                {"data",    predict->images(i)},
            });
    }

    data["stop"] = predict->stopprompts();
    // data["n_probs"] = predict->nprobs();
    //TODO: images,

    return data;
}

// static void parse_options_completion(bool streaming,const backend::PredictOptions* predict, llama_server_context &llama)
// {
//     // https://github.com/ggerganov/llama.cpp/blob/d9b33fe95bd257b36c84ee5769cc048230067d6f/examples/server/server.cpp#L673
//     gpt_params default_params;

//     llama.stream = streaming;
//     llama.params.n_predict = predict->tokens() == 0 ? -1 : predict->tokens();
//     llama.params.sparams.top_k = predict->topk();
//     llama.params.sparams.top_p = predict->topp();
//     llama.params.sparams.tfs_z = predict->tailfreesamplingz();
//     llama.params.sparams.typical_p = predict->typicalp();
//     llama.params.sparams.penalty_last_n = predict->repeat();
//     llama.params.sparams.temp = predict->temperature();
//     llama.params.sparams.penalty_repeat = predict->penalty();
//     llama.params.sparams.penalty_present = predict->presencepenalty();
//     llama.params.sparams.penalty_freq = predict->frequencypenalty();
//     llama.params.sparams.mirostat = predict->mirostat();
//     llama.params.sparams.mirostat_tau = predict->mirostattau();
//     llama.params.sparams.mirostat_eta = predict->mirostateta();
//     llama.params.sparams.penalize_nl = predict->penalizenl();
//     llama.params.n_keep = predict->nkeep();
//     llama.params.seed = predict->seed();
//     llama.params.sparams.grammar = predict->grammar();
//     // llama.params.n_probs = predict->
//     llama.params.prompt = predict->prompt();

//     llama.params.sparams.logit_bias.clear();

//     if (predict->ignoreeos())
//     {
//         llama.params.sparams.logit_bias[llama_token_eos(llama.model)] = -INFINITY;
//     }

//     // const auto &logit_bias = body.find("logit_bias");
//     // if (logit_bias != body.end() && logit_bias->is_array())
//     // {
//     //     const int n_vocab = llama_n_vocab(llama.model);
//     //     for (const auto &el : *logit_bias)
//     //     {
//     //         if (el.is_array() && el.size() == 2 && el[0].is_number_integer())
//     //         {
//     //             llama_token tok = el[0].get<llama_token>();
//     //             if (tok >= 0 && tok < n_vocab)
//     //             {
//     //                 if (el[1].is_number())
//     //                 {
//     //                     llama.params.logit_bias[tok] = el[1].get<float>();
//     //                 }
//     //                 else if (el[1].is_boolean() && !el[1].get<bool>())
//     //                 {
//     //                     llama.params.logit_bias[tok] = -INFINITY;
//     //                 }
//     //             }
//     //         }
//     //     }
//     // }

//     llama.params.antiprompt.clear();
//     for (const std::string& stopPrompt : predict->stopprompts()) {
//     if (!stopPrompt.empty())
//             {
//                 llama.params.antiprompt.push_back(stopPrompt);
//             }
//     }
// }

static void params_parse(const backend::ModelOptions* request,
                                gpt_params & params) {
   
    // this is comparable to: https://github.com/ggerganov/llama.cpp/blob/d9b33fe95bd257b36c84ee5769cc048230067d6f/examples/server/server.cpp#L1809

    params.model = request->modelfile();
    if (!request->mmproj().empty()) {
    // get the directory of modelfile
      std::string model_dir = params.model.substr(0, params.model.find_last_of("/\\"));
      params.mmproj = model_dir + "/"+ request->mmproj();
    }
    //  params.model_alias ??
    params.model_alias =  request->modelfile();
    params.n_ctx = request->contextsize();
    params.memory_f16 = request->f16memory();
    params.n_threads = request->threads();
    params.n_gpu_layers = request->ngpulayers();
    params.n_batch = request->nbatch();
    // Set params.n_parallel by environment variable (LLAMA_PARALLEL), defaults to 1
    //params.n_parallel = 1;
    const char *env_parallel = std::getenv("LLAMACPP_PARALLEL");
    if (env_parallel != NULL) {
        params.n_parallel = std::stoi(env_parallel);
    } else {
        params.n_parallel = 1;
    }

    // TODO: Add yarn

    if (!request->tensorsplit().empty()) {
        std::string arg_next = request->tensorsplit();

        // split string by , and /
        const std::regex regex{ R"([,/]+)" };
        std::sregex_token_iterator it{ arg_next.begin(), arg_next.end(), regex, -1 };
        std::vector<std::string> split_arg{ it, {} };

        GGML_ASSERT(split_arg.size() <= LLAMA_MAX_DEVICES);

        for (size_t i_device = 0; i_device < LLAMA_MAX_DEVICES; ++i_device) {
            if (i_device < split_arg.size()) {
                params.tensor_split[i_device] = std::stof(split_arg[i_device]);
            }
            else {
                params.tensor_split[i_device] = 0.0f;
            }
        }
    }

    if (!request->maingpu().empty()) {
        params.main_gpu = std::stoi(request->maingpu());
    }
    if (!request->loraadapter().empty() && !request->lorabase().empty()) {
     float scale_factor = 1.0f;
     if (request->lorascale() != 0.0f) {
        scale_factor = request->lorascale();
     }
     // get the directory of modelfile
     std::string model_dir = params.model.substr(0, params.model.find_last_of("/\\"));
     params.lora_adapter.push_back(std::make_tuple(model_dir + "/"+request->loraadapter(), scale_factor));
     params.lora_base  =  model_dir + "/"+request->lorabase();
    }
    params.use_mlock = request->mlock();
    params.use_mmap = request->mmap();
    params.embedding = request->embeddings();

    if (request->ropescaling() == "none")   { params.rope_scaling_type = LLAMA_ROPE_SCALING_NONE; }
    else if (request->ropescaling() == "yarn")   { params.rope_scaling_type = LLAMA_ROPE_SCALING_YARN; }
    else { params.rope_scaling_type = LLAMA_ROPE_SCALING_LINEAR; }
    if ( request->yarnextfactor() != 0.0f ) {
        params.yarn_ext_factor = request->yarnextfactor();
    }
    if ( request->yarnattnfactor() != 0.0f ) {
        params.yarn_attn_factor = request->yarnattnfactor();
    }
    if ( request->yarnbetafast() != 0.0f ) {
        params.yarn_beta_fast = request->yarnbetafast();
    }
    if ( request->yarnbetaslow() != 0.0f ) {
        params.yarn_beta_slow = request->yarnbetaslow();
    }
    if ( request->ropefreqbase() != 0.0f ) {
        params.rope_freq_base = request->ropefreqbase();
    }
    if ( request->ropefreqscale() != 0.0f ) {
        params.rope_freq_scale = request->ropefreqscale();
    }
}


// GRPC Server start
class BackendServiceImpl final : public backend::Backend::Service {
public:
  grpc::Status Health(ServerContext* context, const backend::HealthMessage* request, backend::Reply* reply) {
    // Implement Health RPC
    reply->set_message("OK");
    return Status::OK;
  }

  grpc::Status LoadModel(ServerContext* context, const backend::ModelOptions* request, backend::Result* result) {
    // Implement LoadModel RPC
    gpt_params params;
    params_parse(request, params);

    llama_backend_init(params.numa);

    // load the model
    if (!llama.load_model(params))
    {
        result->set_message("Failed loading model");
        result->set_success(false);
        return Status::CANCELLED;
    }
    llama.initialize();
    result->set_message("Loading succeeded");
    result->set_success(true);
    loaded_model = true;
    return Status::OK;
  }
  grpc::Status PredictStream(grpc::ServerContext* context, const backend::PredictOptions* request, grpc::ServerWriter<backend::Reply>* writer) override {
        json data = parse_options(true, request, llama);
        const int task_id = llama.request_completion(data, false, false);
        while (true)
        {
            task_result result = llama.next_result(task_id);
            if (!result.error) {
                const std::string str =
                "data: " +
                result.result_json.dump(-1, ' ', false, json::error_handler_t::replace) +
                "\n\n";
                LOG_VERBOSE("data stream", {
                    { "to_send", str }
                });

                backend::Reply reply;
                // print it
                std::string completion_text = result.result_json.value("content", "");

                reply.set_message(completion_text);

                // Send the reply
                writer->Write(reply);

                if (result.stop) {
                    break;
                }
            } else {
                break;
            }
        }

        return grpc::Status::OK;
    }


    grpc::Status Predict(ServerContext* context, const backend::PredictOptions* request, backend::Reply* reply) {
        json data = parse_options(false, request, llama);
        const int task_id = llama.request_completion(data, false, false);
        std::string completion_text;
        task_result result = llama.next_result(task_id);
        if (!result.error && result.stop) {
            completion_text = result.result_json.value("content", "");
            reply->set_message(completion_text);
        }
        else
        {
            return grpc::Status::OK;
        }

        return grpc::Status::OK;
    }
};

void RunServer(const std::string& server_address) {
  BackendServiceImpl service;

  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;
  server->Wait();
}

int main(int argc, char** argv) {
  std::string server_address("localhost:50051");

  // Define long and short options
  struct option long_options[] = {
      {"addr", required_argument, nullptr, 'a'},
      {nullptr, 0, nullptr, 0}
  };

  // Parse command-line arguments
  int option;
  int option_index = 0;
  while ((option = getopt_long(argc, argv, "a:", long_options, &option_index)) != -1) {
    switch (option) {
      case 'a':
        server_address = optarg;
        break;
      default:
        std::cerr << "Usage: " << argv[0] << " [--addr=<address>] or [-a <address>]" << std::endl;
        return 1;
    }
  }

   // run the HTTP server in a thread - see comment below
    std::thread t([&]()
            {
                RunServer(server_address);
                return 0;
            });


    //);
    start_llama_server();
                        std::cout << "stopping" << std::endl;

    t.join();

    llama_backend_free();
  return 0;
}
