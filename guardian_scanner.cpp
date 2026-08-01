#include <windows.h>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <winrt/Windows.AI.MachineLearning.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Storage.h>
#include <winrt/base.h>

namespace {

using namespace winrt;
using namespace Windows::AI::MachineLearning;
using namespace Windows::Graphics::Imaging;
using namespace Windows::Media;
using namespace Windows::Storage;

std::string JsonEscape(const std::string_view input) {
    std::ostringstream output;
    for (const unsigned char character : input) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20U) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned int>(character) << std::dec;
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    return output.str();
}

std::string Utf8(const std::wstring_view value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return {};
    const int sourceLength = static_cast<int>(value.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), sourceLength,
                                           nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string output(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), sourceLength,
                            output.data(), bytes, nullptr, nullptr) != bytes) return {};
    return output;
}

LearningModelDeviceKind ParseDevice(const std::wstring_view value) noexcept {
    if (value == L"cpu") return LearningModelDeviceKind::Cpu;
    if (value == L"gpu") return LearningModelDeviceKind::DirectXHighPerformance;
    return LearningModelDeviceKind::Default;
}

struct Arguments {
    std::wstring model;
    std::wstring image;
    std::wstring device{L"auto"};
};

bool ParseArguments(const int count, wchar_t** values, Arguments& output) {
    for (int index = 1; index + 1 < count; index += 2) {
        const std::wstring_view key(values[index]);
        const std::wstring value(values[index + 1]);
        if (key == L"--model") output.model = value;
        else if (key == L"--image") output.image = value;
        else if (key == L"--device") output.device = value;
        else return false;
    }
    return count >= 5 && output.model.size() > 3U && output.image.size() > 3U
        && (output.device == L"auto" || output.device == L"cpu" || output.device == L"gpu");
}

std::vector<float> Evaluate(const Arguments& arguments, std::string& inputName,
                            std::string& outputName, std::uint64_t& durationMs) {
    init_apartment(apartment_type::multi_threaded);
    const auto model = LearningModel::LoadFromFilePath(arguments.model);
    hstring imageInput;
    for (const auto& feature : model.InputFeatures()) {
        if (feature.Kind() == LearningModelFeatureKind::Image) {
            imageInput = feature.Name();
            break;
        }
    }
    hstring tensorOutput;
    for (const auto& feature : model.OutputFeatures()) {
        if (feature.Kind() == LearningModelFeatureKind::Tensor) {
            tensorOutput = feature.Name();
            break;
        }
    }
    if (imageInput.empty() || tensorOutput.empty()) {
        throw hresult_error(E_INVALIDARG, L"model_contract_requires_image_input_and_tensor_output");
    }

    const auto sourceFile = StorageFile::GetFileFromPathAsync(arguments.image).get();
    const auto sourceStream = sourceFile.OpenAsync(FileAccessMode::Read).get();
    const auto decoder = BitmapDecoder::CreateAsync(sourceStream).get();
    const auto bitmap = decoder.GetSoftwareBitmapAsync(
        BitmapPixelFormat::Bgra8, BitmapAlphaMode::Ignore).get();
    const auto frame = VideoFrame::CreateWithSoftwareBitmap(bitmap);

    const auto started = std::chrono::steady_clock::now();
    const LearningModelSession session{
        model, LearningModelDevice{ParseDevice(arguments.device)}};
    LearningModelBinding binding{session};
    binding.Bind(imageInput, ImageFeatureValue::CreateFromVideoFrame(frame));
    const auto evaluation = session.EvaluateAsync(binding, L"guardian-local-scan").get();
    const auto rawOutput = evaluation.Outputs().Lookup(tensorOutput);
    const auto tensor = rawOutput.try_as<TensorFloat>();
    if (!tensor) throw hresult_error(E_INVALIDARG, L"model_contract_output_must_be_tensor_float");
    const auto values = tensor.GetAsVectorView();
    if (values.Size() < 2U || values.Size() > 64U) {
        throw hresult_error(E_INVALIDARG, L"model_contract_score_count_invalid");
    }
    std::vector<float> scores;
    scores.reserve(values.Size());
    for (const float score : values) {
        if (!std::isfinite(score)) throw hresult_error(E_INVALIDARG, L"model_score_not_finite");
        scores.push_back(score);
    }
    durationMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count());
    inputName = Utf8(imageInput.c_str());
    outputName = Utf8(tensorOutput.c_str());
    return scores;
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
    Arguments arguments;
    if (!ParseArguments(argc, argv, arguments)) {
        std::cout << R"({"ok":false,"error":"usage"})" << '\n';
        return 2;
    }
    try {
        std::string inputName;
        std::string outputName;
        std::uint64_t durationMs = 0;
        const auto scores = Evaluate(arguments, inputName, outputName, durationMs);
        std::cout << R"({"ok":true,"input":")" << JsonEscape(inputName)
                  << R"(","output":")" << JsonEscape(outputName)
                  << R"(","durationMs":)" << durationMs << R"(,"scores":[)";
        for (std::size_t index = 0; index < scores.size(); ++index) {
            if (index > 0) std::cout << ',';
            std::cout << std::setprecision(8) << scores[index];
        }
        std::cout << "]}\n";
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::cout << R"({"ok":false,"error":")" << JsonEscape(Utf8(error.message().c_str()))
                  << R"(","hresult":)" << static_cast<std::int64_t>(error.code()) << "}\n";
    } catch (const std::exception& error) {
        std::cout << R"({"ok":false,"error":")" << JsonEscape(error.what()) << "\"}\n";
    } catch (...) {
        std::cout << R"({"ok":false,"error":"unknown"})" << '\n';
    }
    return 1;
}
