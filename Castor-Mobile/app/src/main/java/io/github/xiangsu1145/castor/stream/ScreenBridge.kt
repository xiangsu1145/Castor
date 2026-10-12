package io.github.xiangsu1145.castor.stream

import java.nio.ByteBuffer

object ScreenBridge {
    init {
        try {
            System.loadLibrary("ndi")
        } catch (e: Throwable) {
            android.util.Log.e("ScreenBridge", "加载 libndi.so 失败: ${e.message}")
        }
        System.loadLibrary("screenbridge")
    }

    /**
     * NDI 发送端原生初始化
     * @param fps NDI 帧率元数据（真实速率由采集端驱动）
     */
    external fun nativeNdiInit(senderName: String, fps: Int): Boolean

    /**
     * NDI 发送一帧 RGBA 画面
     */
    external fun nativeNdiSend(src: ByteBuffer, width: Int, height: Int, rowStride: Int): Boolean

    /**
     * 通过 NDI metadata 通道上报手机端真实分阶段耗时（XML 字符串）
     */
    external fun nativeNdiSendMeta(xml: String): Boolean

    /**
     * 释放 NDI 发送端资源
     */
    external fun nativeNdiRelease()

    /**
     * 从 RGBA 全屏帧中裁出 region 并编码为 JPEG（libjpeg-turbo SIMD 加速）
     * @param src ImageReader plane[0] 的直接缓冲区（RGBA_8888）
     * @return 裸 JPEG 字节
     */
    external fun nativeEncode(
        src: ByteBuffer,
        rowStride: Int,
        srcWidth: Int,
        srcHeight: Int,
        regionX: Int,
        regionY: Int,
        regionW: Int,
        regionH: Int,
        quality: Int,
    ): ByteArray?

    external fun nativeRelease()
}
