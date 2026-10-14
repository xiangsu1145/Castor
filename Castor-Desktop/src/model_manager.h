#pragma once

#include <string>
#include <vector>
#include <memory>

namespace CastorAI {

// 单个类别的专属检测配置
struct ClassConfig {
    int id = 0;
    std::string name;
    float conf = 0.50f; // 类别专属置信度阈值 (默认 0.50)
    float iou = 0.50f;  // 类别专属 IOU 阈值 (默认 0.50)
};

// 模型项
struct ModelItem {
    std::string id;            // 唯一标识 (例如 "model_1712345678")
    std::string name;          // 模型名字 (用户指定)
    std::string relativePath;  // 相对 .castor 目录或相对路径
    std::string fullPath;      // 缓存后的模型完整路径
    std::string originalPath;  // 导入时的原始文件路径
    int width = 640;           // 输入分辨率宽 (默认 640)
    int height = 640;          // 输入分辨率高 (默认 640)
    float globalNms = 0.45f;   // 总的 NMS 阈值 (默认 0.45)
    std::vector<ClassConfig> classes; // 类别列表及其专属参数
};

// 全局应用配置 (对应 .castor/config.json)
struct AppConfig {
    std::string activeModelId; // 当前选中的模型 ID
    float defaultNms = 0.45f;  // 总的 NMS 默认 0.45
    float defaultConf = 0.50f; // 默认置信度 0.50
    float defaultIou = 0.50f;  // 默认 IOU 0.50
};

// 待导入模型的临时状态
struct PendingImport {
    bool isOpen = false;
    std::string originalFilePath;
    char modelNameInput[128] = "";
    int detectedWidth = 640;
    int detectedHeight = 640;
    bool hasMetadata = false;
    std::vector<std::string> detectedClasses;
    char manualClassesText[2048] = ""; // 未解析到时供用户手动输入（每行一个类别）
};

class ModelManager {
public:
    static ModelManager& Instance();

    // 初始化：创建 .castor 目录并加载 config.json / models.json
    bool Initialize();

    // 保存配置
    void SaveConfig();
    void SaveModels();

    // 模型列表查询与切换
    const std::vector<ModelItem>& GetModels() const { return models_; }
    std::vector<ModelItem>& GetModels() { return models_; }
    ModelItem* GetActiveModel();
    const ModelItem* GetActiveModel() const;
    const std::string& GetActiveModelId() const { return appConfig_.activeModelId; }
    void SetActiveModelId(const std::string& id);

    // 删除模型
    bool DeleteModel(const std::string& modelId);

    // 打开 Windows 原生文件选择对话框选择 .onnx
    bool OpenOnnxFileDialog(std::string& outPath, void* parentHwnd = nullptr);

    // 解析 ONNX 文件的元数据 (输入分辨率、类别列表)
    bool ParseOnnxMetadata(const std::string& filePath, int& outW, int& outH, std::vector<std::string>& outClasses);

    // 触发导入流程
    void TriggerImportFlow(void* parentHwnd = nullptr);

    // 确认导入并持久化
    bool FinalizeImport(const PendingImport& pending);

    // 路径查询
    std::string GetExeDir() const { return exeDir_; }
    std::string GetCastorDir() const { return castorDir_; }
    std::string GetModelsCacheDir() const { return modelsCacheDir_; }

    // 待导入状态
    PendingImport& GetPendingImport() { return pendingImport_; }

private:
    ModelManager() = default;

    std::string exeDir_;
    std::string castorDir_;
    std::string modelsCacheDir_;
    std::string configJsonPath_;
    std::string modelsJsonPath_;

    AppConfig appConfig_;
    std::vector<ModelItem> models_;
    PendingImport pendingImport_;
};

} // namespace CastorAI
