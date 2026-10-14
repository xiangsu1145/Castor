package io.github.xiangsu1145.castor.stream

import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.projection.MediaProjection
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import android.view.Surface
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 方案 B：极致低延迟硬件 H.264 UDP 直推引擎
 * - Surface 零拷贝捕获直出 (0ms 显存直连)
 * - MediaCodec Baseline / 0-Latency / CBR 硬件编码 (2~3ms)
 * - UDP 极速分片直推 (消除 TCP 队头阻塞，Wi-Fi 空中传输 0.5ms)
 * - 双向低延迟反馈 (自动响应 PC 端的 RTT PING 与 IDR 刷新请求)
 * - 支持 GPU 零拷贝单 VirtualDisplay 分流 (中心 1:1 原画 + 外围低画质)
 */
class H264UdpStreamer(
    private val mediaProjection: MediaProjection,
    private val targetIp: String,
    private val targetPort: Int = 27183,
    private val width: Int = 1080,
    private val height: Int = 1528,
    private val fps: Int = 60,
    private val bitrateMbps: Int = 18,
    private val densityDpi: Int = 400,
    private val centerRoiSize: Int = 0
) {
    companion object {
        private const val TAG = "H264UdpStreamer"
        private const val MIME_TYPE = MediaFormat.MIMETYPE_VIDEO_AVC

        // 协议魔数定义
        private const val MAGIC_H264   = 0x34363248 // "H264" Little-Endian
        private const val MAGIC_PING   = 0x474E4950 // "PING"
        private const val MAGIC_PONG   = 0x474E4F50 // "PONG"
        private const val MAGIC_IDR    = 0x52444921 // "!IDR"

        private const val HEADER_SIZE  = 24
        private const val FRAG_PAYLOAD = 1400
        private const val STAT_INTERVAL_MS = 1000L
    }

    private val isStreaming = AtomicBoolean(false)
    private var mediaCodec: MediaCodec? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var udpSocket: DatagramSocket? = null
    private var pcAddress: InetAddress? = null

    // 中心 1:1 原画独立硬件编码管线 (发往 UDP 27192)
    private var roiMediaCodec: MediaCodec? = null
    private var roiWorkerThread: Thread? = null
    private var cachedRoiSpsPps: ByteArray? = null

    // GPU 零拷贝分流器 (单 VirtualDisplay 避免 Android 14 安全崩溃)
    private var gpuSplitter: GpuFrameSplitter? = null

    private var workerThread: Thread? = null
    private var echoThread: Thread? = null

    private var cachedSpsPps: ByteArray? = null

    var onStatusChanged: ((Boolean, String) -> Unit)? = null
    var onStatsUpdated: ((fps: Float, bitrateKbps: Float) -> Unit)? = null

    fun start() {
        if (isStreaming.getAndSet(true)) return

        workerThread = Thread({
            android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_DISPLAY)
            try {
                Log.i(TAG, "正在启动投屏硬件推流 -> $targetIp:$targetPort (规格: ${width}x${height} @ ${fps}FPS, 码率: ${bitrateMbps}Mbps)")
                onStatusChanged?.invoke(true, "正在连接电脑 $targetIp:$targetPort...")

                pcAddress = InetAddress.getByName(targetIp)
                udpSocket = DatagramSocket().apply {
                    sendBufferSize = 4 * 1024 * 1024
                    receiveBufferSize = 64 * 1024
                }

                // 启动 PC 反馈通道监听 (响应 PING/PONG 测距与 IDR 关键帧刷新请求)
                startBackchannelListener()

                setupCodecAndVirtualDisplay()
                drainEncoder()

            } catch (e: Exception) {
                Log.e(TAG, "推流过程异常: ${e.message}", e)
                onStatusChanged?.invoke(false, "推流停止: ${e.message}")
                stop()
            }
        }, "CastorH264Streamer").apply {
            start()
        }
    }

    private var lastIdrRequestTime = 0L

    fun requestKeyFrame() {
        val now = SystemClock.elapsedRealtime()
        if (now - lastIdrRequestTime < 500) {
            return // 500ms 限速，避免网络短暂波动期间重复狂刷 200KB 的超大 IDR 帧
        }
        lastIdrRequestTime = now
        try {
            val params = Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
            }
            mediaCodec?.setParameters(params)
            roiMediaCodec?.setParameters(params)
            Log.d(TAG, "已触发硬件 IDR 关键帧即时刷新 (全景+中心原画)")
        } catch (e: Exception) {
            Log.w(TAG, "请求 IDR 失败: ${e.message}")
        }
    }

    /**
     * 动态调节画质码率 (实时生效，无需重启推流)
     */
    fun updateBitrate(newBitrateMbps: Int) {
        val bps = (newBitrateMbps * 1000 * 1000).coerceIn(4_000_000, 45_000_000)
        try {
            val params = Bundle().apply {
                putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, bps)
            }
            mediaCodec?.setParameters(params)
            Log.i(TAG, "已动态更新编码码率: ${newBitrateMbps} Mbps ($bps bps)")
        } catch (e: Exception) {
            Log.w(TAG, "动态更新码率失败: ${e.message}")
        }
    }

    private fun startBackchannelListener() {
        echoThread = Thread({
            val buf = ByteArray(64)
            val packet = DatagramPacket(buf, buf.size)
            val socket = udpSocket ?: return@Thread

            while (isStreaming.get()) {
                try {
                    socket.receive(packet)
                    if (packet.length >= 4) {
                        val magic = ByteBuffer.wrap(packet.data, 0, 4).order(ByteOrder.LITTLE_ENDIAN).int
                        if (magic == MAGIC_PING && packet.length >= 16) {
                            // 将 PING 变为 PONG，原样反射送回给 PC 源端口
                            ByteBuffer.wrap(packet.data, 0, 4).order(ByteOrder.LITTLE_ENDIAN).putInt(MAGIC_PONG)
                            socket.send(packet)
                        } else if (magic == MAGIC_IDR) {
                            // PC 端检测到连续丢包，请求立刻输出关键帧
                            requestKeyFrame()
                        }
                    }
                } catch (e: Exception) {
                    if (!isStreaming.get()) break
                }
            }
        }, "CastorH264Echo").apply {
            isDaemon = true
            start()
        }
    }

    private fun createEncoder(w: Int, h: Int, bps: Int): MediaCodec {
        val format = MediaFormat.createVideoFormat(MIME_TYPE, w, h).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bps)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 3) // 3 秒刷新关键帧，避免频繁大帧打爆 Wi-Fi
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)

            // 1. Android 标准低延迟与帧率同步
            setIntegerSafe(MediaFormat.KEY_PRIORITY, 0)
            setIntegerSafe(MediaFormat.KEY_LATENCY, 0)
            setIntegerSafe(MediaFormat.KEY_OPERATING_RATE, fps)
            setIntegerSafe(MediaFormat.KEY_CAPTURE_RATE, fps)

            // 2. 彻底禁止 B 帧（单向参考消除重排延迟与宏块撕裂）
            setIntegerSafe("max-bframes", 0)
            setIntegerSafe("max-b-frames", 0)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                setIntegerSafe(MediaFormat.KEY_MAX_B_FRAMES, 0)
            }

            // 3. 高通骁龙私有极低延迟扩展 (QTI Low-Latency Extensions)
            setIntegerSafe("vendor.qti-ext-enc-low-latency.enable", 1)
            setIntegerSafe("vendor.qti-ext-enc-venc-low-latency.enable", 1)
            setIntegerSafe("vendor.qti-ext-enc-preprocess-rotate.enable", 0)

            // 4. 联发科/通用厂商私有低延迟 Key 与 IDR 头前置
            setIntegerSafe("low-latency", 1)
            setIntegerSafe("latency", 0)
            setIntegerSafe("priority", 0)
            setIntegerSafe("max-bitrate", bps)
            setIntegerSafe("prepend-sps-pps-to-idr-frames", 1)
        }
        return MediaCodec.createEncoderByType(MIME_TYPE).apply {
            configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        }
    }

    private fun setupCodecAndVirtualDisplay() {
        // 宽高按偶数对齐 (保持 3000x2120 等真实物理分辨率原生点对点，不产生黑边)
        val alignedW = ((width + 1) / 2) * 2
        val alignedH = ((height + 1) / 2) * 2

        Log.i(TAG, "正在创建背景 MediaCodec 硬件编码器: ${alignedW}x${alignedH} @ ${bitrateMbps}Mbps CBR, ${fps}FPS")
        mediaCodec = createEncoder(alignedW, alignedH, bitrateMbps * 1000 * 1000)
        val fullInputSurface = mediaCodec!!.createInputSurface()
        mediaCodec!!.start()

        val alignedRoi = if (centerRoiSize > 0) (((centerRoiSize + 15) / 16) * 16).coerceIn(100, 1000) else 0
        var roiInputSurface: Surface? = null

        if (alignedRoi > 0) {
            val roiBps = 18 * 1000 * 1000 // 18 Mbps 超高码率密度，保证 640x640 晶莹剔透无压缩伪影
            Log.i(TAG, "正在创建中心 1:1 原画 MediaCodec 硬件编码器: ${alignedRoi}x${alignedRoi} @ 18Mbps CBR, ${fps}FPS")
            roiMediaCodec = createEncoder(alignedRoi, alignedRoi, roiBps)
            roiInputSurface = roiMediaCodec!!.createInputSurface()
            roiMediaCodec!!.start()
        }

        val virtualDisplaySurface: Surface
        if (alignedRoi > 0 && roiInputSurface != null) {
            // 核心技术突破：使用 GPU 零拷贝单 VirtualDisplay 分流器
            gpuSplitter = GpuFrameSplitter(
                fullSurface = fullInputSurface,
                fullWidth = alignedW,
                fullHeight = alignedH,
                roiSurface = roiInputSurface,
                roiWidth = alignedRoi,
                roiHeight = alignedRoi
            )
            virtualDisplaySurface = gpuSplitter!!.start()

            // 启动中心 1:1 原画独立轮询推流线程 (发往 targetPort + 2 = 27192)
            roiWorkerThread = Thread({
                android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_DISPLAY)
                try {
                    drainRoiEncoder(alignedRoi, alignedRoi)
                } catch (e: Exception) {
                    Log.w(TAG, "中心原画推流线程退出: ${e.message}")
                }
            }, "CastorH264RoiStreamer").apply {
                isDaemon = true
                start()
            }
        } else {
            virtualDisplaySurface = fullInputSurface
        }

        // 无论是否启用中心原画，均严格只调用一次 createVirtualDisplay (杜绝 Android 14 崩溃)
        virtualDisplay = mediaProjection.createVirtualDisplay(
            "CastorH264Stream",
            alignedW, alignedH, densityDpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            virtualDisplaySurface, null, null
        )

        Log.i(TAG, "GPU 硬件分流器与 MediaCodec 编码器已就绪: 背景 ${alignedW}x${alignedH}, 中心原画 $alignedRoi")
        onStatusChanged?.invoke(true, if (alignedRoi > 0) "投屏推流中: 全景+${alignedRoi}p中心原画 @ ${fps}FPS" else "投屏推流中: ${alignedW}x${alignedH} @ ${fps}FPS")
    }

    private fun MediaFormat.setIntegerSafe(key: String, value: Int) {
        try {
            setInteger(key, value)
        } catch (_: Exception) {}
    }

    private fun drainEncoder() {
        val bufferInfo = MediaCodec.BufferInfo()
        var frameSeq = 0
        var lastStatsTime = System.currentTimeMillis()
        var framesCount = 0
        var totalBytes = 0L

        val socket = udpSocket ?: return
        val addr = pcAddress ?: return

        val sendPacketBuf = ByteArray(HEADER_SIZE + FRAG_PAYLOAD)
        val sendHeaderBb = ByteBuffer.wrap(sendPacketBuf).order(ByteOrder.LITTLE_ENDIAN)
        val packet = DatagramPacket(sendPacketBuf, sendPacketBuf.size, addr, targetPort)

        Log.i(TAG, "drainEncoder 启动成功，正在轮询 MediaCodec 输出...")

        while (isStreaming.get()) {
            val outputBufferIndex = mediaCodec?.dequeueOutputBuffer(bufferInfo, 10000) ?: -1
            if (outputBufferIndex >= 0) {
                val outputBuffer = mediaCodec?.getOutputBuffer(outputBufferIndex)
                if (outputBuffer != null && bufferInfo.size > 0) {
                    outputBuffer.position(bufferInfo.offset)
                    outputBuffer.limit(bufferInfo.offset + bufferInfo.size)

                    val payloadBytes = ByteArray(bufferInfo.size)
                    outputBuffer.get(payloadBytes)

                    val isConfig = (bufferInfo.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
                    val isKeyFrame = (bufferInfo.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0

                    if (isConfig) {
                        cachedSpsPps = payloadBytes
                        Log.i(TAG, "收到 SPS/PPS 配置头数据: ${payloadBytes.size} 字节")
                    } else if (isKeyFrame) {
                        Log.i(TAG, "收到 IDR 关键帧数据: ${payloadBytes.size} 字节")
                    }

                    // 如果是关键帧且之前缓存了 SPS/PPS，拼接保证独立可解 Annex-B
                    val finalFrameData = if (isKeyFrame && cachedSpsPps != null && !isConfig) {
                        val merged = ByteArray(cachedSpsPps!!.size + payloadBytes.size)
                        System.arraycopy(cachedSpsPps!!, 0, merged, 0, cachedSpsPps!!.size)
                        System.arraycopy(payloadBytes, 0, merged, cachedSpsPps!!.size, payloadBytes.size)
                        merged
                    } else {
                        payloadBytes
                    }

                    val ptsUs = SystemClock.elapsedRealtimeNanos() / 1000L
                    var flags = 0
                    if (isKeyFrame) flags = flags or 0x01
                    if (isConfig) flags = flags or 0x02

                    // 拆分为 MTU 1400 字节分片，走纯 UDP 发送
                    val frameLen = finalFrameData.size
                    val fragCount = (frameLen + FRAG_PAYLOAD - 1) / FRAG_PAYLOAD

                    for (i in 0 until fragCount) {
                        val off = i * FRAG_PAYLOAD
                        val len = minOf(FRAG_PAYLOAD, frameLen - off)

                        sendHeaderBb.clear()
                        sendHeaderBb.putInt(MAGIC_H264)
                        sendHeaderBb.putInt(frameSeq)
                        sendHeaderBb.putShort(i.toShort())
                        sendHeaderBb.putShort(fragCount.toShort())
                        sendHeaderBb.putShort(len.toShort())
                        sendHeaderBb.putShort(flags.toShort())
                        sendHeaderBb.putLong(ptsUs)

                        System.arraycopy(finalFrameData, off, sendPacketBuf, HEADER_SIZE, len)
                        packet.length = HEADER_SIZE + len
                        socket.send(packet)

                        totalBytes += HEADER_SIZE + len

                        // 对分片数较多的大关键帧（>16分片），每 16 包微让步 20 微秒，防止瞬间脉冲打爆路由器/网卡缓冲
                        if (fragCount > 16 && (i and 15) == 15) {
                            java.util.concurrent.locks.LockSupport.parkNanos(20_000L)
                        }
                    }

                    frameSeq++
                    framesCount++
                }
                mediaCodec?.releaseOutputBuffer(outputBufferIndex, false)
            } else if (outputBufferIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                Log.i(TAG, "MediaCodec 硬件编码器输出格式就绪: ${mediaCodec?.outputFormat}")
            }

            // 定时统计刷新
            val now = System.currentTimeMillis()
            val dt = now - lastStatsTime
            if (dt >= STAT_INTERVAL_MS) {
                val realFps = (framesCount * 1000.0f) / dt
                val kbps = (totalBytes * 8.0f) / (dt / 1000.0f) / 1000.0f
                onStatsUpdated?.invoke(realFps, kbps)
                Log.d(TAG, "推流统计: ${String.format("%.1f", realFps)} FPS, ${String.format("%.1f", kbps / 1000.0f)} Mbps, 总帧数: $frameSeq")

                framesCount = 0
                totalBytes = 0L
                lastStatsTime = now
            }
        }
    }

    private fun drainRoiEncoder(roiW: Int, roiH: Int) {
        val bufferInfo = MediaCodec.BufferInfo()
        var roiFrameSeq = 0

        val socket = udpSocket ?: return
        val addr = pcAddress ?: return
        val roiPort = targetPort + 2 // 目标端口 27192

        val sendPacketBuf = ByteArray(HEADER_SIZE + FRAG_PAYLOAD)
        val sendHeaderBb = ByteBuffer.wrap(sendPacketBuf).order(ByteOrder.LITTLE_ENDIAN)
        val packet = DatagramPacket(sendPacketBuf, sendPacketBuf.size, addr, roiPort)

        Log.i(TAG, "drainRoiEncoder 启动成功，正在轮询中心 1:1 原画输出 (${roiW}x${roiH} -> $roiPort)...")

        while (isStreaming.get()) {
            val outputBufferIndex = roiMediaCodec?.dequeueOutputBuffer(bufferInfo, 10000) ?: -1
            if (outputBufferIndex >= 0) {
                val outputBuffer = roiMediaCodec?.getOutputBuffer(outputBufferIndex)
                if (outputBuffer != null && bufferInfo.size > 0) {
                    outputBuffer.position(bufferInfo.offset)
                    outputBuffer.limit(bufferInfo.offset + bufferInfo.size)

                    val payloadBytes = ByteArray(bufferInfo.size)
                    outputBuffer.get(payloadBytes)

                    val isConfig = (bufferInfo.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
                    val isKeyFrame = (bufferInfo.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0

                    if (isConfig) {
                        cachedRoiSpsPps = payloadBytes
                    }

                    val finalFrameData = if (isKeyFrame && cachedRoiSpsPps != null && !isConfig) {
                        val merged = ByteArray(cachedRoiSpsPps!!.size + payloadBytes.size)
                        System.arraycopy(cachedRoiSpsPps!!, 0, merged, 0, cachedRoiSpsPps!!.size)
                        System.arraycopy(payloadBytes, 0, merged, cachedRoiSpsPps!!.size, payloadBytes.size)
                        merged
                    } else {
                        payloadBytes
                    }

                    val ptsUs = SystemClock.elapsedRealtimeNanos() / 1000L
                    var flags = 0
                    if (isKeyFrame) flags = flags or 0x01
                    if (isConfig) flags = flags or 0x02

                    val frameLen = finalFrameData.size
                    val fragCount = (frameLen + FRAG_PAYLOAD - 1) / FRAG_PAYLOAD

                    for (i in 0 until fragCount) {
                        val off = i * FRAG_PAYLOAD
                        val len = minOf(FRAG_PAYLOAD, frameLen - off)

                        sendHeaderBb.clear()
                        sendHeaderBb.putInt(MAGIC_H264)
                        sendHeaderBb.putInt(roiFrameSeq)
                        sendHeaderBb.putShort(i.toShort())
                        sendHeaderBb.putShort(fragCount.toShort())
                        sendHeaderBb.putShort(len.toShort())
                        sendHeaderBb.putShort(flags.toShort())
                        sendHeaderBb.putLong(ptsUs)

                        System.arraycopy(finalFrameData, off, sendPacketBuf, HEADER_SIZE, len)
                        packet.length = HEADER_SIZE + len
                        socket.send(packet)
                    }

                    roiFrameSeq++
                }
                roiMediaCodec?.releaseOutputBuffer(outputBufferIndex, false)
            }
        }
    }

    fun stop() {
        if (!isStreaming.getAndSet(false)) return

        try { virtualDisplay?.release() } catch (_: Exception) {}
        virtualDisplay = null

        try { gpuSplitter?.release() } catch (_: Exception) {}
        gpuSplitter = null

        try {
            roiMediaCodec?.stop()
            roiMediaCodec?.release()
        } catch (_: Exception) {}
        roiMediaCodec = null

        try {
            mediaCodec?.stop()
            mediaCodec?.release()
        } catch (_: Exception) {}
        mediaCodec = null

        try { udpSocket?.close() } catch (_: Exception) {}
        udpSocket = null

        try { roiWorkerThread?.interrupt() } catch (_: Exception) {}
        roiWorkerThread = null
        try { workerThread?.interrupt() } catch (_: Exception) {}
        workerThread = null
        try { echoThread?.interrupt() } catch (_: Exception) {}
        echoThread = null

        onStatusChanged?.invoke(false, "推流已停止")
    }
}
