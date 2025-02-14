#include "absl/flags/parse.h"
#include "absl/flags/flag.h"
#include "absl/flags/usage.h"
#include "absl/flags/internal/commandlineflag.h"
#include "absl/flags/internal/private_handle_accessor.h"
#include "absl/flags/reflection.h"
#include "absl/flags/usage_config.h"
#include "absl/memory/memory.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/delegates/hexagon/hexagon_delegate.h"
#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/interpreter_builder.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/model_builder.h"
#include "tensorflow/lite/testing/util.h"
#include "tensorflow/lite/tools/benchmark/benchmark_utils.h"
#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/model.h"
#include "tensorflow/lite/optional_debug_tools.h"
#include "kenlm/lm/ngram_query.hh"
#include "wave/file.h"
#include "tensorflow/lite/minimal_logging.h"

#define FATAL_ERRORS(errormsg) {fprintf(stderr, "FATAL ERROR (line %d): %s\n", __LINE__, (errormsg)); exit(1);}
#define FATAL_ERROR(errormsg) FATAL_ERRORS((errormsg).c_str())

ABSL_FLAG(std::string, target_file, "INVALID_PATH", "Path to the 16kHz WAV to analyze.");
ABSL_FLAG(std::string, data_folder_path, "/usr/share/tevr_asr_tool", "Path to the data folder.");
ABSL_FLAG(bool, use_language_model, true, "use the language model to boost recognition of common words");

const char* tokens[]= {"", " ", " ", "chen", "sche", "lich", "isch", "icht", "iche", "eine", "rden", "tion", "urde", "haft", "eich", "rung",
                       "chte", "ssen", "chaf", "nder", "tlic", "tung", "eite", "iert", "sich", "ngen", "erde", "scha", "nden", "unge", "lung",
                       "mmen", "eren", "ende", "inde", "erun", "sten", "iese", "igen", "erte", "iner", "tsch", "keit", "der", "die", "ter",
                       "und", "ein", "ist", "den", "ten", "ber", "ver", "sch", "ung", "ste", "ent", "ach", "nte", "auf", "ben", "eit", "des",
                       "ers", "aus", "das", "von", "ren", "gen", "nen", "lle", "hre", "mit", "iel", "uch", "lte", "ann", "lie", "men", "dem",
                       "and", "ind", "als", "sta", "elt", "ges", "tte", "ern", "wir", "ell", "war", "ere", "rch", "abe", "len", "ige", "ied",
                       "ger", "nnt", "wei", "ele", "och", "sse", "end", "all", "ahr", "bei", "sie", "ede", "ion", "ieg", "ege", "auc", "che",
                       "rie", "eis", "vor", "her", "ang", "f\u00fcr", "ass", "uss", "tel", "er", "in", "ge", "en", "st", "ie", "an", "te",
                       "be", "re", "zu", "ar", "es", "ra", "al", "or", "ch", "et", "ei", "un", "le", "rt", "se", "is", "ha", "we", "at", "me",
                       "ne", "ur", "he", "au", "ro", "ti", "li", "ri", "eh", "im", "ma", "tr", "ig", "el", "um", "la", "am", "de", "so", "ol",
                       "tz", "il", "on", "it", "sc", "sp", "ko", "na", "pr", "ni", "si", "fe", "wi", "ns", "ke", "ut", "da", "gr", "eu", "mi",
                       "hr", "ze", "hi", "ta", "ss", "ng", "sa", "us", "ba", "ck", "em", "kt", "ka", "ve", "fr", "bi", "wa", "ah", "gt", "di",
                       "ab", "fo", "to", "rk", "as", "ag", "gi", "hn", "s", "t", "n", "m", "r", "l", "f", "e", "a", "b", "d", "h", "k", "g",
                       "o", "i", "u", "w", "p", "z", "\u00e4", "\u00fc", "v", "\u00f6", "j", "c", "y", "x", "q", "\u00e1", "\u00ed",
                       "\u014d", "\u00f3", "\u0161", "\u00e9", "\u010d", "?" };

const int TOKEN_ID_END_OF_SENTENCE = 1;
const int TOKEN_ID_SPACE = 2;

TfLiteRegistration* Register_ERF();

class LanguageModelBeam {
public:
    float logp;
    std::string text;
    int last_token_idx;

    LanguageModelBeam(const std::string &text, float logp, int last_token_idx) : logp(logp), text(text), last_token_idx(last_token_idx) {}

    void AddLogp(float add_me) {
        if(logp > add_me)
            logp = logp + std::log(1.0 + std::exp(add_me-logp));
        else
            logp = add_me + std::log(1.0 + std::exp(logp-add_me));
    }
};

class LanguageModelDecoder {
public:
    lm::ngram::TrieModel* language_model;
    std::vector<LanguageModelBeam> beams;
    std::map<std::string,LanguageModelBeam> new_beams;
    std::map<std::string, float> cache;

    const float ALPHA = 0.7f;
    const float BETA = 0.75f;
    const float MIN_TOKEN_LOGP = -5.0f;
    const float KENLM_TO_LOGITS = M_LN10; // KenLM is base 10, logits are base e
    const float UNK_LOGP_OFFSET = -10.0;

    void AddToken(int token_idx, float token_logp) {
        for(LanguageModelBeam const& old_beam : beams) {
            if( token_idx == old_beam.last_token_idx ) {
                AddOrSumBeam(old_beam.text, old_beam.logp + token_logp, token_idx);
            } else {
                std::string new_text = ConcatWithoutDuplicateSpaces(old_beam.text, token_idx);
                float language_model_score_difference = GetScoreForText(new_text) - GetScoreForText(old_beam.text);
                AddOrSumBeam(new_text, old_beam.logp + token_logp + language_model_score_difference, token_idx);
            }
        }
    }

    std::string ConcatWithoutDuplicateSpaces(std::string const& prefix, int token_idx) {
        if(prefix.empty()) return tokens[token_idx];
        if(prefix.back() == ' ' and token_idx == TOKEN_ID_SPACE) return prefix;
        return prefix + tokens[token_idx];
    }

    void AddOrSumBeam(std::string const& text, float logp, int last_token_idx) {
        std::string cache_key = text + std::string("|") + std::string(tokens[last_token_idx]);
        std::map<std::string,LanguageModelBeam>::iterator existing_beam = new_beams.find(cache_key);
        if(existing_beam != new_beams.end()) {
            existing_beam->second.AddLogp(logp);
        } else {
            new_beams.emplace(cache_key, LanguageModelBeam(text, logp, last_token_idx));
        }
    }

    void Reduce() {
        beams.clear();
        for(auto addme : new_beams) beams.push_back(addme.second);
        new_beams.clear();
        std::sort(beams.begin(),beams.end(), [](LanguageModelBeam const& a, LanguageModelBeam const& b) {
            return a.logp > b.logp;
        });
        unsigned long full_beam_count = beams.size();
        if( full_beam_count > 500)
            beams.erase(beams.begin()+500, beams.end());
        TFLITE_LOG_PROD(tflite::TFLITE_LOG_INFO, "[%9d beams] \"%s\" @ %f ...", full_beam_count, beams[0].text.c_str(), beams[0].logp);
    }

    float GetScoreForText(std::string const& text) {
        std::map<std::string, float>::iterator cached_result = cache.find(text);
        if(cached_result != cache.end()) return cached_result->second;

        float score = CalculateScoreForText(text);
        cache[text] = score;
        return score;
    }

    float CalculateScoreForText(std::string const& text) {
        float score = 0.0;
        lm::ngram::TrieModel::State state = language_model->BeginSentenceState();
        size_t current_position = 0;
        size_t next_space = 0;
        while ((next_space = text.find(' ', current_position)) != std::string::npos) {
            AddScoreForWord(&state, &score, text.substr(current_position, next_space - current_position));
            current_position = next_space + 1;
        }
        // ignore the last (incomplete) word
        // AddScoreForWord(&state, &score, text.substr(current_position));
        return score;
    }

    void AddScoreForWord(lm::ngram::State *state, float *score, std::string const &word) {
        if(word.empty() || (word.length()==1 && word[0]==' ')) return;
        lm::ngram::SortedVocabulary const& vocabulary = language_model->GetVocabulary();
        const lm::WordIndex idx = vocabulary.Index(word);
        lm::ngram::State out_state;
        const float raw_word_score = language_model->Score(*state, idx, out_state);
        float word_score = raw_word_score * KENLM_TO_LOGITS * ALPHA + BETA;
        if(idx == vocabulary.NotFound()) word_score += UNK_LOGP_OFFSET;
        *state = out_state;
        *score += word_score;
    }

};

int main(int argc, char** argv) {
    ::tflite::LogToStderr();
    absl::SetProgramUsageMessage("Call this program with a --target_file parameter to perform German Automated Speech Recognition on a 16kHz PCM WAV (no float). (c) 2022 Hajo Nils Krabbenhöft @ hajo.me");
    absl::ParseCommandLine(argc, argv);

    const std::string& target_file = absl::GetFlag(FLAGS_target_file);
    const std::string& data_folder_path = absl::GetFlag(FLAGS_data_folder_path);
    const bool& use_language_model = absl::GetFlag(FLAGS_use_language_model);

    TFLITE_LOG_PROD(tflite::TFLITE_LOG_INFO, "Loading WAV ...");

    wave::File wav_file;
    wave::Error wav_error = wav_file.Open(target_file.c_str(), wave::kIn);
    if ( wav_error == wave::kInvalidFormat) FATAL_ERROR(std::string("WAV has invalid format: ") + target_file);
    if (wav_error) FATAL_ERROR(std::string("Could not open WAV file: ") + target_file + std::string(" Make sure to specify a --target_file parameter."));

    if( wav_file.channel_number() != 1) FATAL_ERRORS("WAV file is not mono");
    if( wav_file.sample_rate() != 16000) FATAL_ERRORS("WAV file is not 16kHz");

    std::vector<float> wave_data;
    wav_error = wav_file.Read(&wave_data);
    if (wav_error) FATAL_ERROR(std::string("Could not read WAV file ") + target_file);

    int data_length = wave_data.size();

    for(int i=0;i<4;i++) {
        const std::string &model_path = data_folder_path + std::string("/acoustic_model_0")+std::to_string(i)+std::string(".tflite");
        TFLITE_LOG_PROD(tflite::TFLITE_LOG_INFO, "Loading %s ...", model_path.c_str());
        std::unique_ptr<tflite::FlatBufferModel> acoustic_model = tflite::FlatBufferModel::BuildFromFile(model_path.c_str());
        if(acoustic_model == nullptr) FATAL_ERROR(std::string("Could not load ")+model_path);

        TFLITE_LOG_PROD(tflite::TFLITE_LOG_INFO, "Building interpreter for %s ...", model_path.c_str());
        std::unique_ptr<tflite::Interpreter> acoustic_interpreter;
        tflite::ops::builtin::BuiltinOpResolver resolver;
        //tflite::ops::builtin::BuiltinOpResolverWithoutDefaultDelegates resolver;
        resolver.AddCustom("FlexErf", Register_ERF());
        tflite::InterpreterBuilder builder(*acoustic_model, resolver);
        builder(&acoustic_interpreter);
        if(acoustic_interpreter == nullptr) FATAL_ERROR(std::string("Could not create interpreter for acoustic model ")+std::to_string(i));

        if( i == 0) {
            if( acoustic_interpreter->ResizeInputTensorStrict(0, {1, data_length}) != kTfLiteOk ) FATAL_ERRORS("Could not resize audio input tensor.");
        } else {
            if( acoustic_interpreter->ResizeInputTensorStrict(0, {1, data_length, 1280}) != kTfLiteOk ) FATAL_ERRORS("Could not resize hidden state input tensor.");
        }
        if( acoustic_interpreter->AllocateTensors() != kTfLiteOk) FATAL_ERRORS("Could not allocate tensors for acoustic model.");

        float* audio_input = acoustic_interpreter->typed_input_tensor<float>(0);
        int copy_size = data_length * (i==0 ? 1 : 1280);
        for(int t=0;t<copy_size;t++) audio_input[t] = wave_data[t];

        TFLITE_LOG_PROD(tflite::TFLITE_LOG_INFO, "Invoking %s ...", model_path.c_str());
        if( acoustic_interpreter->Invoke() != kTfLiteOk) FATAL_ERRORS("Could not invoke acoustic model.");

        TfLiteTensor* output = acoustic_interpreter->output_tensor(0);
        if(output->dims->size != 3) FATAL_ERRORS("Wrong output dims");
        if(output->dims->data[0] != 1) FATAL_ERRORS("Wrong output dim 0");
        int expected_dim2 = i==3 ? 256 : 1280;
        if(output->dims->data[2] != expected_dim2) FATAL_ERRORS("Wrong output dim 2");

        data_length = output->dims->data[1];
        wave_data.clear();
        float* logit_output = acoustic_interpreter->typed_output_tensor<float>(0);
        copy_size = data_length * expected_dim2;
        for(int t=0;t<copy_size;t++) wave_data.emplace_back(logit_output[t]);
    }

    // #include "debug_logits.h"

    lm::ngram::Config config;
    const std::string &lm_path = data_folder_path + std::string("/language_model.bin");
    TFLITE_LOG_PROD(tflite::TFLITE_LOG_INFO, "Loading language model %s ...", lm_path.c_str());
    lm::ngram::TrieModel language_model(lm_path.c_str(), config);

    int word_idx = language_model.GetVocabulary().Index(StringPiece("mückenstiche"));
    if(word_idx != 68501) FATAL_ERRORS("Language model vocabulary is wrong.");

    if(!use_language_model) {
        int last_token = 0;
        for( int t = 0; t < data_length; t++ ) {
            int max_idx = 0;
            double max_logit = -5e17;
            for( int i = 0; i < 256; i++ ) {
                float const &v = wave_data[t * 256 + i];
                if( v > max_logit ) {
                    max_logit = v;
                    max_idx = i;
                }
            }
            if( max_idx != last_token ) {
                last_token = max_idx;
                std::cout << tokens[max_idx];
            }
            std::cout << std::endl;
        }
    } else {
        LanguageModelDecoder decoder;
        decoder.language_model = &language_model;
        decoder.beams.emplace_back("",0.0,0);

        for( int t = 0; t < data_length; t++ ) {
            for( int token_idx = 0; token_idx < 256; token_idx++ ) {
                float const &token_logp = wave_data[t * 256 + token_idx];
                if( token_logp < decoder.MIN_TOKEN_LOGP ) continue;
                decoder.AddToken(token_idx, token_logp);
            }
            decoder.Reduce();
        }
        decoder.AddToken(TOKEN_ID_END_OF_SENTENCE, 0.0);
        decoder.Reduce();

        std::cout << decoder.beams[0].text << std::endl;
    }
}
