package io.github.xiangsu1145.castor.stream

import android.content.Context
import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.os.SystemClock
import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.Inet4Address
import java.net.NetworkInterface
import java.util.concurrent.atomic.AtomicBoolean

class NdiStreamer(
    private val context: Context,
    private val mediaProjection: MediaProjection,
    private val width: Int = 1080,
    private val height: Int = 1920,
    private val fps: Int = 60,
    private val densityDpi: Int = 400,
    private val streamName: String = "Castor-Android-Phone"
) {
    companion object {
        private const val TAG = "NdiStreamer"
        private const val LAT_TAG = "CastorLat"
        private const val BEACON_PORT = 5990
    }

    private val isStreaming = AtomicBoolean(false)
    private var imageReader: ImageReader? = null
    private var virtualDisplay: VirtualDisplay? = null
    private var handlerThread: HandlerThread? = null
    private var handler: Handler? = null

    private var multicastLock: WifiManager.MulticastLock? = null
    private var wifiLock: WifiManager.WifiLock? = null
    private var beaconThread: Thread? = null

    var onStatusChanged: ((Boolean, String) -> Unit)? = null
    var onStatsUpdated: ((fps: Float, bitrateKbps: Float) -> Unit)? = null

    private var framesSent = 0
    private var lastFpsTime = 0L
    private var lastCallbackNs = 0L
    // 分阶段真实耗时累计 (ns)，每秒汇总后清零
    private var sumAcquireNs = 0L
    private var sumSendNs = 0L
    private var sumIntervalNs = 0L
    private var intervalSamples = 0

    fun start() {
        if (isStreaming.getAndSet(true)) return

        try {
            // 1. 获取 WiFi 组播锁与低延迟锁 (确保局域网 mDNS 发现广播畅通无阻)
            val wifiManager = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
            try {
                multicastLock = wifiManager?.createMulticastLock("CastorNdiMulticast")?.apply {
                    setReferenceCounted(true)
                    acquire()
                }
            } catch (_: Exception) {}

            try {
                wifiLock = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    wifiManager?.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "CastorNdiWifiLock")
                } else {
                    @Suppress("DEPRECATION")
                    wifiManager?.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "CastorNdiWifiLock")
                }
                wifiLock?.apply {
                    setReferenceCounted(false)
                    acquire()
                }
            } catch (_: Exception) {}

            // 2. 初始化底层 NDI 发送器 (帧率元数据随流固定)
            val initSuccess = ScreenBridge.nativeNdiInit(streamName, fps)
            if (!initSuccess) {
                onStatusChanged?.invoke(false, "NDI 初始化失败")
                stop()
                return
            }

            // 3. 启动后台处理线程与 ImageReader (直接 RGBA_8888 硬件 Surface 接收)
            //    采集/发送线程提到显示级优先级，避免被系统调度到小核导致掉帧
            handlerThread = HandlerThread("CastorNdiHandler", Process.THREAD_PRIORITY_URGENT_DISPLAY).apply { start() }
            handler = Handler(handlerThread!!.looper)

            val isSending = AtomicBoolean(false)
            imageReader = ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 2)
            imageReader?.setOnImageAvailableListener({ reader ->
                if (!isStreaming.get()) return@setOnImageAvailableListener
                val tEnter = SystemClock.elapsedRealtimeNanos()
                val image = reader.acquireLatestImage() ?: return@setOnImageAvailableListener
                if (!isSending.compareAndSet(false, true)) {
                    // 上一帧仍在发送中，立即丢弃，杜绝排队延迟，保证永远直发最新画面
                    image.close()
                    return@setOnImageAvailableListener
                }
                try {
                    val tAcquire = SystemClock.elapsedRealtimeNanos()
                    if (lastCallbackNs != 0L) {
                        sumIntervalNs += (tEnter - lastCallbackNs)
                        intervalSamples++
                    }
                    lastCallbackNs = tEnter
                    val plane = image.planes[0]
                    val buffer = plane.buffer
                    val rowStride = plane.rowStride

                    ScreenBridge.nativeNdiSend(buffer, image.width, image.height, rowStride)

                    val tSend = SystemClock.elapsedRealtimeNanos()
                    framesSent++
                    sumAcquireNs += (tAcquire - tEnter)
                    sumSendNs += (tSend - tAcquire)

                    val now = SystemClock.elapsedRealtime()
                    if (now - lastFpsTime >= 1000) {
                        val frames = framesSent
                        val elapsedMs = (now - lastFpsTime).toDouble()
                        val currentFps = if (elapsedMs > 0) (frames * 1000.0 / elapsedMs).toFloat() else 0f
                        val acquireMs = if (frames > 0) sumAcquireNs / frames / 1e6 else 0.0
                        val sendMs = if (frames > 0) sumSendNs / frames / 1e6 else 0.0
                        val intervalMs = if (intervalSamples > 0) sumIntervalNs / intervalSamples / 1e6 else 0.0
                        val estKbps = currentFps * (width * height * 0.2f) * 8f / 1000f
                        if (frames > 0) {
                            Log.i(LAT_TAG, String.format(
                                "fps=%.1f interval=%.2fms acquire=%.2fms convert+send=%.2fms size=%dx%d",
                                currentFps, intervalMs, acquireMs, sendMs, width, height))
                            // 真实分阶段耗时经 NDI metadata 上报给 PC (无需时钟同步的"时长")
                            val xml = String.format(
                                "<castor seq=\"%d\" fps=\"%.1f\" interval_ms=\"%.2f\" acquire_ms=\"%.2f\" send_ms=\"%.2f\" w=\"%d\" h=\"%d\"/>",
                                frames, currentFps, intervalMs, acquireMs, sendMs, width, height)
                            ScreenBridge.nativeNdiSendMeta(xml)
                        }
                        onStatsUpdated?.invoke(currentFps, estKbps)
                        framesSent = 0
                        sumAcquireNs = 0L
                        sumSendNs = 0L
                        sumIntervalNs = 0L
                        intervalSamples = 0
                        lastFpsTime = now
                    }
                } catch (e: Exception) {
                    Log.e(TAG, "NDI 发送异常: ${e.message}")
                } finally {
                    image.close()
                    isSending.set(false)
                }
            }, handler)

            // 4. 创建虚拟屏幕直连 ImageReader
            virtualDisplay = mediaProjection.createVirtualDisplay(
                "CastorNDIDisplay",
                width, height, densityDpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                imageReader!!.surface, null, handler
            )

            lastFpsTime = SystemClock.elapsedRealtime()
            startBeaconThread()
            onStatusChanged?.invoke(true, "NDI 极速推流中 [$streamName] (电脑已可秒级自动发现)")
            Log.i(TAG, "NDI 极速推流启动成功: ${width}x${height} @ ${fps}FPS")

        } catch (e: Exception) {
            Log.e(TAG, "启动 NDI 失败: ${e.message}", e)
            onStatusChanged?.invoke(false, "启动失败: ${e.message}")
            stop()
        }
    }

    private fun startBeaconThread() {
        stopBeaconThread()
        beaconThread = Thread({
            var socket: DatagramSocket? = null
            try {
                socket = DatagramSocket()
                socket.broadcast = true
                val broadcastAddr = InetAddress.getByName("255.255.255.255")
                while (isStreaming.get()) {
                    val localIp = getLocalIpAddress()
                    if (localIp != null) {
                        val msg = "CASTOR_NDI_BEACON:$streamName:$localIp"
                        val bytes = msg.toByteArray(Charsets.UTF_8)
                        val packet = DatagramPacket(bytes, bytes.size, broadcastAddr, BEACON_PORT)
                        socket.send(packet)
                    }
                    Thread.sleep(1000)
                }
            } catch (_: Exception) {
            } finally {
                socket?.close()
            }
        }, "NdiBeaconThread").apply {
            isDaemon = true
            start()
        }
    }

    private fun stopBeaconThread() {
        beaconThread?.interrupt()
        beaconThread = null
    }

    private fun getLocalIpAddress(): String? {
        try {
            val interfaces = NetworkInterface.getNetworkInterfaces() ?: return null
            for (nIf in interfaces) {
                if (nIf.isLoopback || !nIf.isUp) continue
                val addrs = nIf.inetAddresses ?: continue
                for (addr in addrs) {
                    if (!addr.isLoopbackAddress && addr is Inet4Address) {
                        val host = addr.hostAddress ?: continue
                        if (host != "127.0.0.1") return host
                    }
                }
            }
        } catch (_: Exception) {}
        return null
    }

    fun stop() {
        if (!isStreaming.getAndSet(false)) return

        stopBeaconThread()

        try {
            virtualDisplay?.release()
            virtualDisplay = null
        } catch (_: Exception) {}

        try {
            imageReader?.close()
            imageReader = null
        } catch (_: Exception) {}

        try {
            handlerThread?.quitSafely()
            handlerThread = null
            handler = null
        } catch (_: Exception) {}

        try {
            ScreenBridge.nativeNdiRelease()
        } catch (_: Exception) {}

        try {
            if (multicastLock?.isHeld == true) multicastLock?.release()
            if (wifiLock?.isHeld == true) wifiLock?.release()
        } catch (_: Exception) {}
        multicastLock = null
        wifiLock = null

        onStatusChanged?.invoke(false, "NDI 推流已停止")
        Log.i(TAG, "NDI 推流已安全停止")
    }
}
