package io.github.xiangsu1145.castor.stream

import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.projection.MediaProjection
import android.util.Log
import java.io.BufferedOutputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.nio.ByteBuffer
import java.util.concurrent.atomic.AtomicBoolean

class ScreenStreamer(
    private val mediaProjection: MediaProjection,
    private val targetIp: String,
    private val targetPort: Int,
    private val width: Int = 1920,
    private val height: Int = 1080,
    private val fps: Int = 60,
    private val bitrateMbps: Int = 20,
    private val densityDpi: Int = 400
) {
    companion object {
        private const val TAG = "ScreenStreamer"
        private const val MIME_TYPE = MediaFormat.MIMETYPE_VIDEO_AVC
    }

    private val isStreaming = AtomicBoolean(false)
    private var mediaCodec: MediaCodec? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var clientSocket: Socket? = null
    private var outputStream: OutputStream? = null

    private var workerThread: Thread? = null

    var onStatusChanged: ((Boolean, String) -> Unit)? = null
    var onStatsUpdated: ((fps: Float, bitrateKbps: Float) -> Unit)? = null

    fun start() {
        if (isStreaming.getAndSet(true)) return

        workerThread = Thread {
            try {
                Log.d(TAG, "正在连接 PC 端: $targetIp:$targetPort ...")
                onStatusChanged?.invoke(true, "正在连接 PC 端...")

                clientSocket = Socket().apply {
                    tcpNoDelay = true
                    sendBufferSize = 1024 * 1024
                    connect(InetSocketAddress(targetIp, targetPort), 3000)
                }
                outputStream = BufferedOutputStream(clientSocket!!.getOutputStream(), 256 * 1024)
                Log.d(TAG, "TCP 套接字连接成功")
                onStatusChanged?.invoke(true, "已连接 PC，正在开启硬件编码器...")

                setupCodecAndVirtualDisplay()
                drainEncoder()

            } catch (e: Exception) {
                Log.e(TAG, "推流过程出现异常: ${e.message}", e)
                onStatusChanged?.invoke(false, "连接断开: ${e.message}")
                stop()
            }
        }.apply {
            name = "Castor-Streamer"
            start()
        }
    }

    private fun setupCodecAndVirtualDisplay() {
        val format = MediaFormat.createVideoFormat(MIME_TYPE, width, height).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrateMbps * 1000 * 1000)
            setInteger(MediaFormat.KEY_FRAME_RATE, fps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1) // 1 秒刷新关键帧

            // 极低延迟核心参数
            setInteger(MediaFormat.KEY_LATENCY, 0)
            setInteger(MediaFormat.KEY_PRIORITY, 0) // 实时最高优先级
            setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileBaseline)
            setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
        }

        mediaCodec = MediaCodec.createEncoderByType(MIME_TYPE).apply {
            configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
        }

        val inputSurface = mediaCodec!!.createInputSurface()
        mediaCodec!!.start()

        virtualDisplay = mediaProjection.createVirtualDisplay(
            "CastorScreenMirror",
            width, height, densityDpi,
            DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            inputSurface, null, null
        )

        Log.d(TAG, "硬件编码器与 VirtualDisplay 创建成功 (分辨率: ${width}x${height}, FPS: $fps)")
        onStatusChanged?.invoke(true, "推流中: ${width}x${height} @ ${fps}FPS")
    }

    private fun drainEncoder() {
        val bufferInfo = MediaCodec.BufferInfo()
        var frameSeq = 0
        var lastStatsTime = System.currentTimeMillis()
        var framesCount = 0
        var totalBytes = 0L

        val headerBuffer = ByteBuffer.allocate(StreamProtocol.HEADER_SIZE)

        while (isStreaming.get()) {
            val outputBufferIndex = mediaCodec?.dequeueOutputBuffer(bufferInfo, 10000) ?: -1
            if (outputBufferIndex >= 0) {
                val outputBuffer = mediaCodec?.getOutputBuffer(outputBufferIndex)
                if (outputBuffer != null && bufferInfo.size > 0) {
                    outputBuffer.position(bufferInfo.offset)
                    outputBuffer.limit(bufferInfo.offset + bufferInfo.size)

                    val payloadSize = bufferInfo.size
                    val timeNs = System.nanoTime()

                    // 打包发送协议帧头
                    headerBuffer.clear()
                    headerBuffer.putInt(StreamProtocol.MAGIC)
                    headerBuffer.put(StreamProtocol.TYPE_VIDEO_FRAME)
                    headerBuffer.put(0.toByte())
                    headerBuffer.putShort(0.toShort())
                    headerBuffer.putInt(frameSeq++)
                    headerBuffer.putInt(payloadSize)

                    val os = outputStream
                    if (os != null) {
                        os.write(headerBuffer.array(), 0, StreamProtocol.HEADER_SIZE)
                        val payloadBytes = ByteArray(payloadSize)
                        outputBuffer.get(payloadBytes)
                        os.write(payloadBytes)
                        os.flush()
                    }

                    framesCount++
                    totalBytes += payloadSize
                }
                mediaCodec?.releaseOutputBuffer(outputBufferIndex, false)
            }

            // 定时统计刷新
            val now = System.currentTimeMillis()
            if (now - lastStatsTime >= 1000) {
                val currentRealFps = (framesCount * 1000.0f) / (now - lastStatsTime)
                val currentBitrateKbps = (totalBytes * 8.0f) / (now - lastStatsTime)
                onStatsUpdated?.invoke(currentRealFps, currentBitrateKbps)

                framesCount = 0
                totalBytes = 0L
                lastStatsTime = now
            }
        }
    }

    fun stop() {
        if (!isStreaming.getAndSet(false)) return

        try {
            virtualDisplay?.release()
            virtualDisplay = null

            mediaCodec?.stop()
            mediaCodec?.release()
            mediaCodec = null

            outputStream?.close()
            outputStream = null

            clientSocket?.close()
            clientSocket = null

            workerThread?.interrupt()
            workerThread = null
        } catch (e: Exception) {
            Log.w(TAG, "清理资源异常: ${e.message}")
        }

        onStatusChanged?.invoke(false, "已停止推流")
    }
}
