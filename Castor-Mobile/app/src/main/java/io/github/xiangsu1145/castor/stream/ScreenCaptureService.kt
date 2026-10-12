package io.github.xiangsu1145.castor.stream

import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Binder
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.util.Log
import androidx.core.app.NotificationCompat

class ScreenCaptureService : Service() {

    companion object {
        const val CHANNEL_ID = "castor_stream_channel"
        const val NOTIFICATION_ID = 1001
        private const val TAG = "ScreenCaptureService"
    }

    private val binder = LocalBinder()
    private var mediaProjection: MediaProjection? = null
    private var ndiStreamer: NdiStreamer? = null
    private var udpStreamer: H264UdpStreamer? = null
    private var h264Probe: H264EncodeProbe? = null
    private var udpTargetIp: String? = null
    private var udpTargetPort: Int = 27190
    private var udpBitrateMbps: Int = 20

    @Volatile private var capturing = false
    @Volatile var targetFps: Int = 60
    private var currentDensityDpi: Int = 320
    private var currentStreamName: String = "Castor-Android-Phone"
    private var currentWidth: Int = 1080
    private var currentHeight: Int = 1920

    private var statusCallback: ((Boolean, String) -> Unit)? = null
    private var statsCallback: ((Float, Float) -> Unit)? = null

    private val mainHandler = Handler(Looper.getMainLooper())

    private val projectionCallback = object : MediaProjection.Callback() {
        override fun onStop() {
            Log.i(TAG, "用户终止了屏幕捕获权限")
            mainHandler.post { stopStreaming() }
        }
    }

    inner class LocalBinder : Binder() {
        fun getService(): ScreenCaptureService = this@ScreenCaptureService
    }

    fun updateFps(fps: Int) {
        this.targetFps = fps.coerceIn(15, 144)
    }

    fun updateResolution(newWidth: Int, newHeight: Int, densityDpi: Int) {
        if (!capturing) return
        this.currentWidth = newWidth
        this.currentHeight = newHeight
        this.currentDensityDpi = densityDpi
        val cbStatus = statusCallback
        val cbStats = statsCallback
        if (cbStatus == null || cbStats == null) return
        // 按当前激活的通道重启，否则切分辨率对私有 UDP 通道无效
        val ip = udpTargetIp
        if (udpStreamer != null && !ip.isNullOrEmpty()) {
            startUdpStream(
                ip, udpTargetPort, currentWidth, currentHeight,
                targetFps, udpBitrateMbps, currentDensityDpi, cbStatus, cbStats
            )
        } else {
            startStreaming(
                currentStreamName, currentWidth, currentHeight, targetFps, currentDensityDpi,
                cbStatus, cbStats
            )
        }
    }

    override fun onBind(intent: Intent?): IBinder = binder

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val notification = buildNotification("Castor 极速无线串流前台服务运行中...")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(
                NOTIFICATION_ID,
                notification,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION
            )
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }

        val resultCode = intent?.getIntExtra("resultCode", Activity.RESULT_CANCELED) ?: Activity.RESULT_CANCELED
        val resultData = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent?.getParcelableExtra("resultData", Intent::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent?.getParcelableExtra("resultData")
        }

        if (resultCode == Activity.RESULT_OK && resultData != null) {
            try {
                val projectionManager = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
                mediaProjection?.stop()
                val projection = projectionManager.getMediaProjection(resultCode, resultData)
                if (projection != null) {
                    projection.registerCallback(projectionCallback, mainHandler)
                    mediaProjection = projection
                    Log.d(TAG, "MediaProjection 初始化成功")
                }
            } catch (e: Exception) {
                Log.e(TAG, "创建 MediaProjection 失败: ${e.message}", e)
            }
        }

        return START_NOT_STICKY
    }

    fun startStreaming(
        streamName: String,
        width: Int,
        height: Int,
        fps: Int,
        densityDpi: Int,
        onStatusChanged: (Boolean, String) -> Unit,
        onStatsUpdated: (Float, Float) -> Unit
    ) {
        this.targetFps = fps.coerceIn(30, 144)
        this.currentDensityDpi = densityDpi
        this.currentStreamName = streamName.ifBlank { "Castor-Android-Phone" }
        this.currentWidth = width
        this.currentHeight = height
        this.statusCallback = onStatusChanged
        this.statsCallback = onStatsUpdated

        var retryCount = 0
        while (mediaProjection == null && retryCount < 10) {
            try { Thread.sleep(50) } catch (_: Exception) {}
            retryCount++
        }

        val projection = mediaProjection ?: run {
            onStatusChanged(false, "错误: 未获取到屏幕截取实例")
            return
        }

        stopCapture()

        try {
            Log.i(TAG, "启动 NDI 广播推流 [${currentStreamName}] (${width}x${height} @ ${fps}FPS)")

            ndiStreamer = NdiStreamer(
                context = applicationContext,
                mediaProjection = projection,
                width = width,
                height = height,
                fps = fps,
                densityDpi = densityDpi,
                streamName = currentStreamName
            ).apply {
                this.onStatusChanged = { running, msg ->
                    mainHandler.post { onStatusChanged(running, msg) }
                }
                this.onStatsUpdated = { currentFps, kbps ->
                    mainHandler.post { onStatsUpdated(currentFps, kbps) }
                }
                start()
            }

            capturing = true
            onStatusChanged(true, "NDI 极速推流中 [${currentStreamName}]")

        } catch (e: Exception) {
            Log.e(TAG, "启动 NDI 捕获失败: ${e.message}", e)
            onStatusChanged(false, "启动失败: ${e.message}")
            stopCapture()
        }
    }

    /**
     * 纯实测探针：跑一遍硬件 H.264 编码管线，量出真实 帧率/编码延迟/码率（不发任何网络数据）。
     */
    fun startH264Probe(
        width: Int,
        height: Int,
        fps: Int,
        densityDpi: Int,
        onStatusChanged: (Boolean, String) -> Unit
    ) {
        this.currentWidth = width
        this.currentHeight = height
        this.currentDensityDpi = densityDpi
        this.targetFps = fps

        var retryCount = 0
        while (mediaProjection == null && retryCount < 10) {
            try { Thread.sleep(50) } catch (_: Exception) {}
            retryCount++
        }
        val projection = mediaProjection ?: run {
            onStatusChanged(false, "错误: 未获取到屏幕截取实例")
            return
        }

        stopCapture()
        Log.i(TAG, "启动 H.264 编码实测探针 (${width}x${height} @ ${fps}fps)")

        h264Probe = H264EncodeProbe(
            mediaProjection = projection,
            width = width,
            height = height,
            fps = fps,
            densityDpi = densityDpi
        ).apply {
            this.onStatus = { running, msg -> mainHandler.post { onStatusChanged(running, msg) } }
            start()
        }
        capturing = true
        onStatusChanged(true, "H.264 编码实测中…（结果见 logcat 标签 H264Probe）")
    }

    /**
     * 私有 UDP H.264 推流：手机硬编 H.264 -> UDP 直推 PC（低延迟 ~10-20Mbps，不占 WiFi 带宽）。
     */
    fun startUdpStream(
        targetIp: String,
        targetPort: Int,
        width: Int,
        height: Int,
        fps: Int,
        bitrateMbps: Int,
        densityDpi: Int,
        onStatusChanged: (Boolean, String) -> Unit,
        onStatsUpdated: (Float, Float) -> Unit
    ) {
        this.targetFps = fps.coerceIn(30, 144)
        this.currentDensityDpi = densityDpi
        this.currentWidth = width
        this.currentHeight = height
        this.statusCallback = onStatusChanged
        this.statsCallback = onStatsUpdated
        this.udpTargetIp = targetIp
        this.udpTargetPort = targetPort
        this.udpBitrateMbps = bitrateMbps
        this.statusCallback = onStatusChanged
        this.statsCallback = onStatsUpdated

        var retryCount = 0
        while (mediaProjection == null && retryCount < 10) {
            try { Thread.sleep(50) } catch (_: Exception) {}
            retryCount++
        }
        val projection = mediaProjection ?: run {
            onStatusChanged(false, "错误: 未获取到屏幕截取实例")
            return
        }

        stopCapture()
        Log.i(TAG, "启动私有 UDP H.264 推流 -> $targetIp:$targetPort (${width}x${height} @ ${fps}fps, ${bitrateMbps}Mbps)")

        udpStreamer = H264UdpStreamer(
            mediaProjection = projection,
            targetIp = targetIp,
            targetPort = targetPort,
            width = width,
            height = height,
            fps = fps,
            bitrateMbps = bitrateMbps,
            densityDpi = densityDpi
        ).apply {
            this.onStatusChanged = { running, msg -> mainHandler.post { onStatusChanged(running, msg) } }
            this.onStatsUpdated = { f, kbps -> mainHandler.post { onStatsUpdated(f, kbps) } }
            start()
        }
        capturing = true
        onStatusChanged(true, "私有 UDP H.264 推流中 -> $targetIp:$targetPort")
    }

    private fun stopCapture() {
        capturing = false
        try {
            ndiStreamer?.stop()
        } catch (_: Exception) {}
        ndiStreamer = null
        try {
            udpStreamer?.stop()
        } catch (_: Exception) {}
        udpStreamer = null
        try {
            h264Probe?.stop()
        } catch (_: Exception) {}
        h264Probe = null
    }

    fun stopStreaming() {
        stopCapture()
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    override fun onDestroy() {
        stopStreaming()
        mediaProjection?.stop()
        mediaProjection = null
        super.onDestroy()
    }

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "Castor 极速无线推流服务",
                NotificationManager.IMPORTANCE_LOW
            )
            val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            manager.createNotificationChannel(channel)
        }
    }

    private fun buildNotification(text: String): Notification {
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("Castor 极低延迟无线推流")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.ic_menu_camera)
            .setOngoing(true)
            .build()
        }
}
