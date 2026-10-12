#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include <d3d11.h>

namespace CastorVideo {

class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();

    // 初始化低延迟解码器 (Windows Media Foundation MFT，支持 D3D11VA 硬件加速)
    bool Init(ID3D11Device* d3dDevice = nullptr);
    void Shutdown();

    // 解码单个 H.264 帧 (Annex-B 格式，包含 00 00 00 01 NALU 起始码)
    // rawNv12=true 时不做 BGRA 转换，outBgra 里放的是原始 NV12（Y 平面 + UV 平面），
    // 行跨度写入 *outStride，供调用方（GPU 路径）自行处理。
    bool DecodeFrame(const uint8_t* data, size_t size, std::vector<uint8_t>& outBgra, int& outW, int& outH,
                     bool rawNv12 = false, int* outStride = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace CastorVideo
