// screenbridge —— 局部裁剪 + MJPEG 编码（libjpeg-turbo, NEON）
//
// 输入 RGBA_8888 全屏帧（ImageReader plane 0），裁出 region 后编码为 JPEG。
// 选 RGBA 而非 YUV_420_888：绕开各厂商 plane/stride 差异，裁剪就是行级 memcpy。
//
// 每帧零动态分配：裁剪缓冲和 JPEG 输出缓冲都预分配复用（JPEG 按 tjBufSize
// 最坏情况预留），返回裸 JPEG，由 UdpSender 拆分片后发送。

#include <jni.h>
#include <android/log.h>
#include <turbojpeg.h>
#include <Processing.NDI.Lib.h>

#include <cstring>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <vector>

#define LOG_TAG "screenbridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

std::mutex g_tjMutex;
tjhandle g_tjCompressor = nullptr;
std::vector<uint8_t> g_cropBuf;
std::vector<uint8_t> g_jpegBuf;

tjhandle getCompressor() {
    if (g_tjCompressor == nullptr) {
        g_tjCompressor = tjInitCompress();
    }
    return g_tjCompressor;
}

}  // namespace

extern "C" JNIEXPORT jbyteArray JNICALL
Java_io_github_xiangsu1145_castor_stream_ScreenBridge_nativeEncode(
        JNIEnv* env, jclass /*clazz*/, jobject srcBuf, jint rowStride,
        jint srcWidth, jint srcHeight,
        jint regionX, jint regionY, jint regionW, jint regionH, jint quality) {
    auto* src = static_cast<const uint8_t*>(env->GetDirectBufferAddress(srcBuf));
    if (src == nullptr) {
        LOGE("src buffer is not direct");
        return nullptr;
    }

    // region 收敛到屏幕范围内
    int rx = std::max(0, std::min(regionX, srcWidth - 1));
    int ry = std::max(0, std::min(regionY, srcHeight - 1));
    int rw = std::min(regionW, srcWidth - rx);
    int rh = std::min(regionH, srcHeight - ry);
    if (rw <= 0 || rh <= 0) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(g_tjMutex);
    tjhandle tj = getCompressor();
    if (tj == nullptr) {
        LOGE("tjInitCompress failed");
        return nullptr;
    }

    // 直接使用原始缓冲区与行跨度，无需行级 memcpy 内存搬运 (零拷贝极速模式)
    const uint8_t* startPixel = src + static_cast<size_t>(ry) * rowStride + static_cast<size_t>(rx) * 4;

    // JPEG 按 tjBufSize 最坏情况预留，tjCompress2 就地写入，无每帧 malloc
    const unsigned long maxJpeg = tjBufSize(rw, rh, TJSAMP_420);
    if (g_jpegBuf.size() < maxJpeg) {
        g_jpegBuf.resize(maxJpeg);
    }
    unsigned char* jpegBuf = g_jpegBuf.data();
    unsigned long jpegSize = maxJpeg;

    // 启用 TJFLAG_FASTDCT (快速整数 DCT 变换)，使 ARM NEON 汇编压缩速度翻倍
    if (tjCompress2(tj, startPixel, rw, rowStride, rh, TJPF_RGBA,
                    &jpegBuf, &jpegSize, TJSAMP_420, quality, TJFLAG_FASTDCT) < 0) {
        LOGE("tjCompress2 failed: %s", tjGetErrorStr2(tj));
        return nullptr;
    }

    // 输出裸 JPEG（分片和帧头由 UdpSender 处理）
    jbyteArray out = env->NewByteArray(static_cast<jsize>(jpegSize));
    if (out != nullptr) {
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(jpegSize),
                                reinterpret_cast<const jbyte*>(jpegBuf));
    }
    return out;
}

extern "C" JNIEXPORT void JNICALL
Java_io_github_xiangsu1145_castor_stream_ScreenBridge_nativeRelease(JNIEnv* /*env*/, jclass /*clazz*/) {
    std::lock_guard<std::mutex> lock(g_tjMutex);
    if (g_tjCompressor != nullptr) {
        tjDestroy(g_tjCompressor);
        g_tjCompressor = nullptr;
    }
    g_cropBuf.clear();
    g_cropBuf.shrink_to_fit();
    g_jpegBuf.clear();
    g_jpegBuf.shrink_to_fit();
}

namespace {
std::mutex g_ndiMutex;
NDIlib_send_instance_t g_ndiSend = nullptr;

// NDI 视频帧率元数据，由 nativeNdiInit 传入（默认 60fps）
int g_frameRateN = 60000;
int g_frameRateD = 1000;

// 双缓冲 Ping-Pong：NDI 异步发送在下次调用前仍引用 p_data，而 ImageReader 只有 2 个
// buffer、不能把 plane 直接挂给 NDI（会 stall 采集），因此逐帧 memcpy 到复用缓冲。
// RGBA 紧密排布，交给 NDI 内部线程压缩，省掉 CPU 色彩转换。
std::vector<uint8_t> g_rgbaPing;
std::vector<uint8_t> g_rgbaPong;
bool g_pingPongToggle = false;
int g_lastW = 0;
int g_lastH = 0;

// 诊断：nativeNdiSend 内部耗时拆解 (memcpy vs NDI async 调用)
int64_t g_diagCopyNs = 0;
int64_t g_diagSendNs = 0;
int g_diagCount = 0;
int64_t g_diagWinStartNs = 0;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_io_github_xiangsu1145_castor_stream_ScreenBridge_nativeNdiInit(
        JNIEnv* env, jclass /*clazz*/, jstring senderName, jint fps) {
    std::lock_guard<std::mutex> lock(g_ndiMutex);
    if (!NDIlib_initialize()) {
        LOGE("NDIlib_initialize failed");
        return JNI_FALSE;
    }

    if (fps >= 1 && fps <= 240) {
        g_frameRateN = static_cast<int>(fps) * 1000;
        g_frameRateD = 1000;
    }

    const char* nameStr = env->GetStringUTFChars(senderName, nullptr);
    NDIlib_send_create_t createSettings{};
    createSettings.p_ndi_name = nameStr ? nameStr : "Castor-Android-Phone";
    createSettings.clock_video = false; // 严禁内部休眠限速，实现零延迟直推
    createSettings.clock_audio = false;
    g_ndiSend = NDIlib_send_create(&createSettings);
    if (nameStr) {
        env->ReleaseStringUTFChars(senderName, nameStr);
    }

    if (!g_ndiSend) {
        LOGE("NDIlib_send_create failed");
        return JNI_FALSE;
    }
    LOGI("NDI Sender initialized successfully with clock_video = false, fps = %d", static_cast<int>(fps));
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_io_github_xiangsu1145_castor_stream_ScreenBridge_nativeNdiSend(
        JNIEnv* env, jclass /*clazz*/, jobject srcBuf,
        jint width, jint height, jint rowStride) {
    std::lock_guard<std::mutex> lock(g_ndiMutex);
    if (!g_ndiSend) return JNI_FALSE;

    auto* src = static_cast<const uint8_t*>(env->GetDirectBufferAddress(srcBuf));
    if (!src) return JNI_FALSE;

    // 1. 确保宽度和高度偶数对齐
    int alignW = (width / 2) * 2;
    int alignH = (height / 2) * 2;
    if (alignW <= 0 || alignH <= 0) return JNI_FALSE;

    // 2. 双缓冲 Ping-Pong 空间预留 (RGBA 4 字节/像素，紧密排布)
    const size_t dstStride = static_cast<size_t>(alignW) * 4;
    const size_t rgbaSize = dstStride * static_cast<size_t>(alignH);
    if (g_rgbaPing.size() < rgbaSize || g_lastW != alignW || g_lastH != alignH) {
        g_rgbaPing.resize(rgbaSize);
        g_rgbaPong.resize(rgbaSize);
        g_lastW = alignW;
        g_lastH = alignH;
    }

    uint8_t* targetBuf = g_pingPongToggle ? g_rgbaPong.data() : g_rgbaPing.data();
    g_pingPongToggle = !g_pingPongToggle;

    auto nowNs = []() -> int64_t {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const int64_t tc0 = nowNs();

    // 3. 逐行拷贝到紧密 RGBA 缓冲 (源 rowStride 可能含对齐填充)，
    //    直接给 NDI 送 RGBA，SHQ 压缩由 NDI 内部线程完成，消除 CPU 色彩转换
    if (static_cast<size_t>(rowStride) == dstStride) {
        std::memcpy(targetBuf, src, rgbaSize);
    } else {
        for (int y = 0; y < alignH; ++y) {
            std::memcpy(targetBuf + static_cast<size_t>(y) * dstStride,
                        src + static_cast<size_t>(y) * rowStride,
                        dstStride);
        }
    }

    const int64_t tc1 = nowNs();

    // 4. 构建标准 NDI RGBA 视频帧
    NDIlib_video_frame_v2_t frame{};
    frame.xres = alignW;
    frame.yres = alignH;
    frame.FourCC = NDIlib_FourCC_video_type_RGBA;
    frame.line_stride_in_bytes = static_cast<int>(dstStride);
    frame.p_data = targetBuf;
    frame.frame_rate_N = g_frameRateN;
    frame.frame_rate_D = g_frameRateD;
    frame.picture_aspect_ratio = static_cast<float>(alignW) / static_cast<float>(alignH > 0 ? alignH : 1);
    frame.frame_format_type = NDIlib_frame_format_type_progressive;
    frame.timecode = NDIlib_send_timecode_synthesize; // 广播级极低延迟实时时钟

    // 5. 异步交付 NDI 内部后台线程池压缩发送，主捕获线程 0 阻塞 (<0.1ms 立即返回)
    const int64_t ts0 = nowNs();
    NDIlib_send_send_video_async_v2(g_ndiSend, &frame);
    const int64_t ts1 = nowNs();

    // 诊断: 拆解 memcpy 与 NDI async 调用各自耗时，每秒打印一次
    g_diagCopyNs += (tc1 - tc0);
    g_diagSendNs += (ts1 - ts0);
    g_diagCount++;
    if (ts1 - g_diagWinStartNs >= 1000000000LL) {
        if (g_diagCount > 0) {
            LOGI("NDI send breakdown(n=%d): memcpy=%.2fms async_call=%.2fms total=%.2fms",
                 g_diagCount,
                 g_diagCopyNs / g_diagCount / 1e6,
                 g_diagSendNs / g_diagCount / 1e6,
                 (g_diagCopyNs + g_diagSendNs) / g_diagCount / 1e6);
        }
        g_diagCopyNs = 0;
        g_diagSendNs = 0;
        g_diagCount = 0;
        g_diagWinStartNs = ts1;
    }

    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_io_github_xiangsu1145_castor_stream_ScreenBridge_nativeNdiRelease(
        JNIEnv* /*env*/, jclass /*clazz*/) {
    std::lock_guard<std::mutex> lock(g_ndiMutex);
    if (g_ndiSend) {
        // 传递 NULL 触发 NDI 内部完成同步刷新，确保上一异步帧处理完毕
        NDIlib_send_send_video_v2(g_ndiSend, nullptr);
        NDIlib_send_destroy(g_ndiSend);
        g_ndiSend = nullptr;
    }
    g_rgbaPing.clear();
    g_rgbaPing.shrink_to_fit();
    g_rgbaPong.clear();
    g_rgbaPong.shrink_to_fit();
    g_lastW = 0;
    g_lastH = 0;
    NDIlib_destroy();
    LOGI("NDI Sender released");
}

// 通过 NDI metadata 通道把手机端"真实"分阶段耗时上报给接收端。
// metadata 是 NDI 连接自带的可靠旁路，不需要额外端口/时钟同步。
extern "C" JNIEXPORT jboolean JNICALL
Java_io_github_xiangsu1145_castor_stream_ScreenBridge_nativeNdiSendMeta(
        JNIEnv* env, jclass /*clazz*/, jstring xml) {
    std::lock_guard<std::mutex> lock(g_ndiMutex);
    if (!g_ndiSend || !xml) return JNI_FALSE;

    const char* str = env->GetStringUTFChars(xml, nullptr);
    if (!str) return JNI_FALSE;

    NDIlib_metadata_frame_t meta{};
    meta.p_data = const_cast<char*>(str);
    meta.length = static_cast<int>(std::strlen(str)) + 1;
    meta.timecode = NDIlib_send_timecode_synthesize;
    NDIlib_send_send_metadata(g_ndiSend, &meta);

    env->ReleaseStringUTFChars(xml, str);
    return JNI_TRUE;
}

