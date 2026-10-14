#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include "model_manager.h"
#include <windows.h>
#include <commdlg.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <regex>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace CastorAI {

static std::string WStringToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string str(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &str[0], sizeNeeded, nullptr, nullptr);
    return str;
}

static std::wstring Utf8ToWString(const std::string& str) {
    if (str.empty()) return L"";
    int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring wstr(sizeNeeded, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstr[0], sizeNeeded);
    return wstr;
}

ModelManager& ModelManager::Instance() {
    static ModelManager s_instance;
    return s_instance;
}

bool ModelManager::Initialize() {
    wchar_t exePathBuf[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
    fs::path exePath(exePathBuf);
    exeDir_ = WStringToUtf8(exePath.parent_path().wstring());

    fs::path castorPath = exePath.parent_path() / ".castor";
    castorDir_ = WStringToUtf8(castorPath.wstring());

    fs::path modelsPath = castorPath / "models";
    modelsCacheDir_ = WStringToUtf8(modelsPath.wstring());

    std::error_code ec;
    fs::create_directories(modelsPath, ec);

    configJsonPath_ = WStringToUtf8((castorPath / "config.json").wstring());
    modelsJsonPath_ = WStringToUtf8((castorPath / "models.json").wstring());

    // 1. 读取或创建 config.json
    if (fs::exists(configJsonPath_)) {
        try {
            std::ifstream f(configJsonPath_);
            if (f.is_open()) {
                json j;
                f >> j;
                appConfig_.activeModelId = j.value("active_model_id", "");
                appConfig_.defaultNms = j.value("global_nms", 0.45f);
                appConfig_.defaultConf = j.value("default_conf", 0.50f);
                appConfig_.defaultIou = j.value("default_iou", 0.50f);
            }
        } catch (...) {}
    } else {
        SaveConfig();
    }

    // 2. 读取或创建 models.json
    if (fs::exists(modelsJsonPath_)) {
        try {
            std::ifstream f(modelsJsonPath_);
            if (f.is_open()) {
                json j;
                f >> j;
                models_.clear();
                if (j.contains("models") && j["models"].is_array()) {
                    for (const auto& item : j["models"]) {
                        ModelItem m;
                        m.id = item.value("id", "");
                        m.name = item.value("name", "未命名模型");
                        m.relativePath = item.value("relative_path", "");
                        m.fullPath = item.value("full_path", "");
                        m.originalPath = item.value("original_path", "");
                        m.width = item.value("width", 640);
                        m.height = item.value("height", 640);
                        m.globalNms = item.value("global_nms", 0.45f);

                        if (item.contains("classes") && item["classes"].is_array()) {
                            for (const auto& c : item["classes"]) {
                                ClassConfig cc;
                                cc.id = c.value("id", 0);
                                cc.name = c.value("name", "");
                                cc.conf = c.value("conf", 0.50f);
                                cc.iou = c.value("iou", 0.50f);
                                m.classes.push_back(cc);
                            }
                        }
                        models_.push_back(m);
                    }
                }
            }
        } catch (...) {}
    } else {
        SaveModels();
    }

    return true;
}

void ModelManager::SaveConfig() {
    try {
        json j;
        j["active_model_id"] = appConfig_.activeModelId;
        j["global_nms"] = appConfig_.defaultNms;
        j["default_conf"] = appConfig_.defaultConf;
        j["default_iou"] = appConfig_.defaultIou;

        std::ofstream f(configJsonPath_);
        if (f.is_open()) {
            f << j.dump(2);
        }
    } catch (...) {}
}

void ModelManager::SaveModels() {
    try {
        json j;
        j["models"] = json::array();

        for (const auto& m : models_) {
            json item;
            item["id"] = m.id;
            item["name"] = m.name;
            item["relative_path"] = m.relativePath;
            item["full_path"] = m.fullPath;
            item["original_path"] = m.originalPath;
            item["width"] = m.width;
            item["height"] = m.height;
            item["global_nms"] = m.globalNms;

            json clsArr = json::array();
            for (const auto& c : m.classes) {
                json cj;
                cj["id"] = c.id;
                cj["name"] = c.name;
                cj["conf"] = c.conf;
                cj["iou"] = c.iou;
                clsArr.push_back(cj);
            }
            item["classes"] = clsArr;

            j["models"].push_back(item);
        }

        std::ofstream f(modelsJsonPath_);
        if (f.is_open()) {
            f << j.dump(2);
        }
    } catch (...) {}
}

ModelItem* ModelManager::GetActiveModel() {
    if (appConfig_.activeModelId.empty()) {
        if (!models_.empty()) {
            return &models_[0];
        }
        return nullptr;
    }
    for (auto& m : models_) {
        if (m.id == appConfig_.activeModelId) {
            return &m;
        }
    }
    if (!models_.empty()) return &models_[0];
    return nullptr;
}

const ModelItem* ModelManager::GetActiveModel() const {
    if (appConfig_.activeModelId.empty()) {
        if (!models_.empty()) {
            return &models_[0];
        }
        return nullptr;
    }
    for (const auto& m : models_) {
        if (m.id == appConfig_.activeModelId) {
            return &m;
        }
    }
    if (!models_.empty()) return &models_[0];
    return nullptr;
}

void ModelManager::SetActiveModelId(const std::string& id) {
    appConfig_.activeModelId = id;
    SaveConfig();
}

bool ModelManager::DeleteModel(const std::string& modelId) {
    auto it = std::find_if(models_.begin(), models_.end(), [&](const ModelItem& m) {
        return m.id == modelId;
    });
    if (it != models_.end()) {
        std::error_code ec;
        fs::remove(Utf8ToWString(it->fullPath), ec);
        models_.erase(it);

        if (appConfig_.activeModelId == modelId) {
            appConfig_.activeModelId = models_.empty() ? "" : models_[0].id;
            SaveConfig();
        }
        SaveModels();
        return true;
    }
    return false;
}

bool ModelManager::OpenOnnxFileDialog(std::string& outPath, void* parentHwnd) {
    wchar_t fileNameBuf[MAX_PATH]{};

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = (HWND)parentHwnd;
    ofn.lpstrFilter = L"ONNX 深度学习模型 (*.onnx)\0*.onnx\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = fileNameBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"选择要导入的 ONNX 模型";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        outPath = WStringToUtf8(fileNameBuf);
        return true;
    }
    return false;
}

bool ModelManager::ParseOnnxMetadata(const std::string& filePath, int& outW, int& outH, std::vector<std::string>& outClasses) {
    outW = 640;
    outH = 640;
    outClasses.clear();

    std::ifstream file(Utf8ToWString(filePath), std::ios::binary);
    if (!file.is_open()) return false;

    // 读取前 1MB 二进制（ONNX metadata_props 和 graph 头部都在最前端）
    const size_t maxScan = 1024 * 1024;
    std::vector<char> buffer(maxScan);
    file.read(buffer.data(), maxScan);
    size_t readBytes = (size_t)file.gcount();
    buffer.resize(readBytes);

    std::string content(buffer.begin(), buffer.end());

    // 1. 扫描 imgsz 分辨率 (如 "[256, 256]" 或 "[640, 640]")
    size_t imgszPos = content.find("imgsz");
    if (imgszPos != std::string::npos) {
        size_t bOpen = content.find('[', imgszPos);
        size_t bClose = content.find(']', imgszPos);
        if (bOpen != std::string::npos && bClose != std::string::npos && bClose > bOpen && (bClose - bOpen) < 40) {
            std::string sub = content.substr(bOpen + 1, bClose - bOpen - 1);
            int w = 0, h = 0;
            if (sscanf_s(sub.c_str(), "%d,%d", &w, &h) == 2 && w > 0 && h > 0) {
                outW = w;
                outH = h;
            } else if (sscanf_s(sub.c_str(), "%d , %d", &w, &h) == 2 && w > 0 && h > 0) {
                outW = w;
                outH = h;
            }
        }
    }

    // 2. 扫描 names (YOLO 模型通常存放于 metadata 中的 names 键)
    size_t namesPos = content.find("names");
    bool foundClasses = false;

    if (namesPos != std::string::npos) {
        // 查找其后的字典 '{' 或列表 '['
        size_t searchLimit = (std::min)(content.size(), namesPos + 8192);
        size_t bracePos = std::string::npos;
        for (size_t i = namesPos; i < searchLimit; ++i) {
            if (content[i] == '{' || content[i] == '[') {
                bracePos = i;
                break;
            }
        }

        if (bracePos != std::string::npos) {
            char openChar = content[bracePos];
            char closeChar = (openChar == '{') ? '}' : ']';
            size_t closePos = content.find(closeChar, bracePos);

            if (closePos != std::string::npos && closePos > bracePos && (closePos - bracePos) < 16384) {
                std::string dictStr = content.substr(bracePos, closePos - bracePos + 1);

                // Python 导出常见格式：{0: 'head', 1: 'body'} 或 {0: "head"}
                // 替换单引号为双引号以兼容 JSON
                std::string jsonStr;
                jsonStr.reserve(dictStr.size() + 32);
                for (size_t i = 0; i < dictStr.size(); ++i) {
                    char c = dictStr[i];
                    if (c == '\'') jsonStr += '\"';
                    else jsonStr += c;
                }

                // 尝试用正则表达式提取字符串值 (更容错)
                std::regex re(R"(['"]([^'"]+)['"])");
                std::sregex_iterator it(dictStr.begin(), dictStr.end(), re);
                std::sregex_iterator end;

                std::map<int, std::string> idToName;
                std::vector<std::string> rawList;

                // 尝试正则匹配 key: 'val'
                std::regex pairRe(R"((\d+)\s*:\s*['"]([^'"]+)['"])");
                std::sregex_iterator pit(dictStr.begin(), dictStr.end(), pairRe);

                if (pit != end) {
                    while (pit != end) {
                        int id = std::stoi((*pit)[1].str());
                        std::string name = (*pit)[2].str();
                        idToName[id] = name;
                        ++pit;
                    }
                    for (const auto& kv : idToName) {
                        outClasses.push_back(kv.second);
                    }
                    if (!outClasses.empty()) foundClasses = true;
                } else {
                    // 若是列表 ['cls1', 'cls2']
                    while (it != end) {
                        std::string val = (*it)[1].str();
                        if (val != "names") {
                            rawList.push_back(val);
                        }
                        ++it;
                    }
                    if (!rawList.empty()) {
                        outClasses = rawList;
                        foundClasses = true;
                    }
                }
            }
        }
    }

    return foundClasses;
}

void ModelManager::TriggerImportFlow(void* parentHwnd) {
    std::string path;
    if (OpenOnnxFileDialog(path, parentHwnd)) {
        pendingImport_.isOpen = true;
        pendingImport_.originalFilePath = path;

        fs::path p(Utf8ToWString(path));
        std::string stem = WStringToUtf8(p.stem().wstring());
        strncpy_s(pendingImport_.modelNameInput, sizeof(pendingImport_.modelNameInput), stem.c_str(), _TRUNCATE);

        int w = 640, h = 640;
        std::vector<std::string> classes;
        bool hasMeta = ParseOnnxMetadata(path, w, h, classes);

        pendingImport_.detectedWidth = w;
        pendingImport_.detectedHeight = h;
        pendingImport_.hasMetadata = hasMeta;
        pendingImport_.detectedClasses = classes;
        pendingImport_.manualClassesText[0] = '\0';
    }
}

bool ModelManager::FinalizeImport(const PendingImport& pending) {
    if (pending.originalFilePath.empty()) return false;

    fs::path orig(Utf8ToWString(pending.originalFilePath));
    if (!fs::exists(orig)) return false;

    // 1. 生成唯一 ID
    auto nowNs = std::chrono::system_clock::now().time_since_epoch().count();
    std::string modelId = "model_" + std::to_string(nowNs);

    // 2. 拷贝到 .castor/models 缓存文件夹中
    std::string safeName = (pending.modelNameInput[0] != '\0') ? pending.modelNameInput : "model";
    for (char& c : safeName) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '\"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }
    std::string destFileName = safeName + "_" + std::to_string(nowNs % 100000) + ".onnx";
    fs::path destPath = fs::path(Utf8ToWString(modelsCacheDir_)) / destFileName;

    std::error_code ec;
    fs::copy_file(orig, destPath, fs::copy_options::overwrite_existing, ec);

    // 3. 构建类别列表
    std::vector<ClassConfig> classes;
    if (pending.hasMetadata && !pending.detectedClasses.empty()) {
        for (size_t i = 0; i < pending.detectedClasses.size(); ++i) {
            ClassConfig cc;
            cc.id = (int)i;
            cc.name = pending.detectedClasses[i];
            cc.conf = 0.50f;
            cc.iou = 0.50f;
            classes.push_back(cc);
        }
    } else {
        // 用户手动填写的类别 (每行一个)
        std::stringstream ss(pending.manualClassesText);
        std::string line;
        int id = 0;
        while (std::getline(ss, line)) {
            // trim
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
            if (!line.empty()) {
                ClassConfig cc;
                cc.id = id++;
                cc.name = line;
                cc.conf = 0.50f;
                cc.iou = 0.50f;
                classes.push_back(cc);
            }
        }
    }

    if (classes.empty()) {
        // 兜底至少有一个通用类别
        ClassConfig cc;
        cc.id = 0;
        cc.name = "target";
        cc.conf = 0.50f;
        cc.iou = 0.50f;
        classes.push_back(cc);
    }

    // 4. 创建 ModelItem
    ModelItem item;
    item.id = modelId;
    item.name = (pending.modelNameInput[0] != '\0') ? pending.modelNameInput : "未命名模型";
    item.relativePath = ".castor/models/" + destFileName;
    item.fullPath = WStringToUtf8(destPath.wstring());
    item.originalPath = pending.originalFilePath;
    item.width = pending.detectedWidth;
    item.height = pending.detectedHeight;
    item.globalNms = 0.45f;
    item.classes = classes;

    models_.push_back(item);
    appConfig_.activeModelId = item.id;

    SaveModels();
    SaveConfig();

    return true;
}

} // namespace CastorAI
