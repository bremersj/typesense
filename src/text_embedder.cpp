#include "text_embedder.h"
#include "text_embedder_manager.h"
#include "logger.h"
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>

TextEmbedder::TextEmbedder(const std::string& model_name) {
    // create environment
    Ort::SessionOptions session_options;
    std::string abs_path = TextEmbedderManager::get_absolute_model_path(model_name);
    LOG(INFO) << "Loading model from disk: " << abs_path;
    session_ = std::make_unique<Ort::Session>(env_, abs_path.c_str(), session_options);
    std::ifstream config_file(TextEmbedderManager::get_absolute_config_path(model_name));
    nlohmann::json config;
    config_file >> config;
    TokenizerType tokenizer_type = TextEmbedderManager::get_tokenizer_type(config);
    auto vocab_path = TextEmbedderManager::get_absolute_vocab_path(model_name, config["vocab_file_name"].get<std::string>());
    if(tokenizer_type == TokenizerType::bert) {
        tokenizer_ = std::make_unique<BertTokenizerWrapper>(vocab_path);
    } else if(tokenizer_type == TokenizerType::distilbert) {
        tokenizer_ = std::make_unique<DistilbertTokenizer>(vocab_path);
    }
    else if(tokenizer_type == TokenizerType::xlm_roberta) {
        tokenizer_ = std::make_unique<XLMRobertaTokenizer>(vocab_path);
    }
    auto output_tensor_count = session_->GetOutputCount();
    for (size_t i = 0; i < output_tensor_count; i++) {
        auto shape = session_->GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() == 3 && shape[0] == -1 && shape[1] == -1 && shape[2] > 0) {
            Ort::AllocatorWithDefaultOptions allocator;
            output_tensor_name = std::string(session_->GetOutputNameAllocated(i, allocator).get());
            break;
        }
    }
}

TextEmbedder::TextEmbedder(const std::string& model_name, const std::string& api_key) {
    LOG(INFO) << "Loading model from remote: " << model_name;
    auto model_namespace = TextEmbedderManager::get_model_namespace(model_name);

    if (model_namespace == "openai") {
        remote_embedder_ = std::make_unique<OpenAIEmbedder>(model_name, api_key);
    } else if (model_namespace == "google") {
        remote_embedder_ = std::make_unique<GoogleEmbedder>(api_key);
    } 
}


std::vector<float> TextEmbedder::mean_pooling(const std::vector<std::vector<float>>& inputs) {

    std::vector<float> pooled_output;
    for (int i = 0; i < inputs[0].size(); i++) {
        float sum = 0;
        for (int j = 0; j < inputs.size(); j++) {
            sum += inputs[j][i];
        }
        pooled_output.push_back(sum / inputs.size());
    }
    return pooled_output;
}

Option<std::vector<float>> TextEmbedder::Embed(const std::string& text) {
    if(is_remote()) {
        return remote_embedder_->Embed(text);
    } else {
        auto encoded_input = tokenizer_->Encode(text);
        // create input tensor object from data values
        Ort::AllocatorWithDefaultOptions allocator;
        Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtAllocatorType::OrtArenaAllocator, OrtMemType::OrtMemTypeDefault);
        std::vector<Ort::Value> input_tensors;
        std::vector<std::vector<int64_t>> input_shapes;
        std::vector<const char*> input_node_names = {"input_ids", "attention_mask"};
        // If model is DistilBERT or sentencepiece, it has 2 inputs, else it has 3 inputs
        if(session_->GetInputCount() == 3) {
            input_node_names.push_back("token_type_ids");
        }
        input_shapes.push_back({1, static_cast<int64_t>(encoded_input.input_ids.size())});
        input_shapes.push_back({1, static_cast<int64_t>(encoded_input.attention_mask.size())});
        if(session_->GetInputCount() == 3) {
            input_shapes.push_back({1, static_cast<int64_t>(encoded_input.token_type_ids.size())});
        }
        input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(memory_info, encoded_input.input_ids.data(), encoded_input.input_ids.size(), input_shapes[0].data(), input_shapes[0].size()));
        input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(memory_info, encoded_input.attention_mask.data(), encoded_input.attention_mask.size(), input_shapes[1].data(), input_shapes[1].size()));
        if(session_->GetInputCount() == 3) {
            input_tensors.push_back(Ort::Value::CreateTensor<int64_t>(memory_info, encoded_input.token_type_ids.data(), encoded_input.token_type_ids.size(), input_shapes[2].data(), input_shapes[2].size()));
        }

        //LOG(INFO) << "Running model";
        // create output tensor object
        std::vector<const char*> output_node_names = {output_tensor_name.c_str()};
        auto output_tensor = session_->Run(Ort::RunOptions{nullptr}, input_node_names.data(), input_tensors.data(), input_tensors.size(), output_node_names.data(), output_node_names.size());
        std::vector<std::vector<float>> output;
        float* data = output_tensor[0].GetTensorMutableData<float>();
        // print output tensor shape
        auto shape = output_tensor[0].GetTensorTypeAndShapeInfo().GetShape();

        for (int i = 0; i < shape[1]; i++) {
            std::vector<float> temp;
            for (int j = 0; j < shape[2]; j++) {
                temp.push_back(data[i * shape[2] + j]);
            }
            output.push_back(temp);
        }
        auto pooled_output = mean_pooling(output);  

        return Option<std::vector<float>>(pooled_output);
    }
}

Option<std::vector<std::vector<float>>> TextEmbedder::batch_embed(const std::vector<std::string>& inputs) {
    std::vector<std::vector<float>> outputs;
    if(!is_remote()) {
        // for now only openai is supported for batch embedding
        for(const auto& input : inputs) {
            outputs.push_back(Embed(input).get());
        }
    } else {
        outputs = std::move(remote_embedder_->batch_embed(inputs).get());
    }
    return Option<std::vector<std::vector<float>>>(outputs);
}

TextEmbedder::~TextEmbedder() {
}


bool TextEmbedder::is_model_valid(const std::string& model_name, unsigned int& num_dims) {
    LOG(INFO) << "Validating model: " << model_name;

    if(TextEmbedderManager::get_instance().is_public_model(model_name)) {
       auto res = TextEmbedderManager::get_instance().download_public_model(model_name);
       if(!res.ok()) {
              LOG(ERROR) << res.error();
              return false;
       }
    }


    Ort::SessionOptions session_options;
    Ort::Env env;
    std::string abs_path = TextEmbedderManager::get_absolute_model_path(TextEmbedderManager::get_model_name_without_namespace(model_name));

    if(!std::filesystem::exists(abs_path)) {
        LOG(ERROR) << "Model file not found: " << abs_path;
        return false;
    }

    if(!TextEmbedderManager::get_instance().is_public_model(model_name)) {
        if(!std::filesystem::exists(TextEmbedderManager::get_absolute_config_path(model_name))) {
            LOG(ERROR) << "Config file not found: " << TextEmbedderManager::get_absolute_config_path(model_name);
            return false;
        }
        std::ifstream config_file(TextEmbedderManager::get_absolute_config_path(model_name));
        nlohmann::json config;
        config_file >> config;
        if(config["model_type"].is_null() || config["vocab_file_name"].is_null()) {
            LOG(ERROR) << "Invalid config file: " << TextEmbedderManager::get_absolute_config_path(model_name);
            return false;
        }

        if(!config["model_type"].is_string() || !config["vocab_file_name"].is_string()) {
            LOG(ERROR) << "Invalid config file: " << TextEmbedderManager::get_absolute_config_path(model_name);
            return false;
        }

        if(!std::filesystem::exists(TextEmbedderManager::get_model_subdir(model_name) + "/" + config["vocab_file_name"].get<std::string>())) {
            LOG(ERROR) << "Vocab file not found: " << TextEmbedderManager::get_model_subdir(model_name) + "/" + config["vocab_file_name"].get<std::string>();
            return false;
        }

        if(config["model_type"].get<std::string>() != "bert" && config["model_type"].get<std::string>() != "xlm_roberta" && config["model_type"].get<std::string>() != "distilbert") {
            LOG(ERROR) << "Invalid model type: " << config["model_type"].get<std::string>();
            return false;
        }
    }

    Ort::Session session(env, abs_path.c_str(), session_options);
    if(session.GetInputCount() != 3 && session.GetInputCount() != 2) {
        LOG(ERROR) << "Invalid model: input count is not 3 or 2";
        return false;
    }
    Ort::AllocatorWithDefaultOptions allocator;
    auto input_ids_name = session.GetInputNameAllocated(0, allocator);
    if (std::strcmp(input_ids_name.get(), "input_ids") != 0) {
        LOG(ERROR) << "Invalid model: input_ids tensor not found";
        return false;
    }

    auto attention_mask_name = session.GetInputNameAllocated(1, allocator);
    if (std::strcmp(attention_mask_name.get(), "attention_mask") != 0) {
        LOG(ERROR) << "Invalid model: attention_mask tensor not found";
        return false;
    }


    if(session.GetInputCount() == 3) {
        auto token_type_ids_name = session.GetInputNameAllocated(2, allocator);
        if (std::strcmp(token_type_ids_name.get(), "token_type_ids") != 0) {
            LOG(ERROR) << "Invalid model: token_type_ids tensor not found";
            return false;
        }
    }

    auto output_tensor_count = session.GetOutputCount();
    bool found_output_tensor = false;
    for (size_t i = 0; i < output_tensor_count; i++) {
        auto shape = session.GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size() == 3 && shape[0] == -1 && shape[1] == -1 && shape[2] > 0) {
            num_dims = shape[2];
            found_output_tensor = true;
            break;
        }
    }

    if (!found_output_tensor) {
        LOG(ERROR) << "Invalid model: Output tensor not found";
        return false;
    }

    return true;
}


Option<bool> TextEmbedder::is_model_valid(const std::string model_name, const std::string api_key, unsigned int& num_dims) {
    auto model_namespace = TextEmbedderManager::get_model_namespace(model_name);

    if(model_namespace == "openai") {
        return OpenAIEmbedder::is_model_valid(model_name, api_key, num_dims);
    } else if(model_namespace == "google") {
        return GoogleEmbedder::is_model_valid(model_name, api_key, num_dims);
    } else {
        return Option<bool>(400, "Invalid model namespace");
    }
}