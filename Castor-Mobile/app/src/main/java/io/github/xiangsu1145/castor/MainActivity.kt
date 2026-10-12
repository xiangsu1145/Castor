package io.github.xiangsu1145.castor

import android.app.Activity
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Point
import android.hardware.display.DisplayManager
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.util.DisplayMetrics
import android.util.TypedValue
import android.view.Display
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.LinearLayout
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.widget.NestedScrollView
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import com.google.android.material.color.MaterialColors
import com.google.android.material.snackbar.Snackbar
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.textfield.TextInputLayout
import com.google.android.material.textview.MaterialTextView
import io.github.xiangsu1145.castor.net.LanDiscovery
import io.github.xiangsu1145.castor.stream.ScreenCaptureService

class MainActivity : AppCompatActivity() {

    private var captureService: ScreenCaptureService? = null
    private var isBound = false

    // 动态控件引用
    private lateinit var statusTextView: MaterialTextView
    private lateinit var ndiNameEditText: TextInputEditText
    private lateinit var actionButton: MaterialButton
    private lateinit var liveStatsText: MaterialTextView

    // 当前推流参数
    private var screenPhysicalW = 2120
    private var screenPhysicalH = 3000
    private var targetWidth = 1080
    private var targetHeight = 1920
    private var targetFps = 60
    private var isStreamingActive = false
    private var pendingProbe = false
    private var useUdp = false          // 传输通道: false=NDI, true=私有 UDP H.264
    private var pcIp: String? = null    // 自动发现的 PC IP
    private var lanDiscovery: LanDiscovery? = null

    // 真实屏幕尺寸：走 DisplayManager 的默认显示，跟随系统旋转（不受本 Activity 竖屏锁定影响）。
    private fun getScreenMetrics(): Pair<Int, Int> {
        return try {
            val dm = getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
            val display = dm.getDisplay(Display.DEFAULT_DISPLAY)
            val p = Point()
            display.getRealSize(p)
            Pair(p.x, p.y)
        } catch (e: Exception) {
            Pair(screenPhysicalW, screenPhysicalH)
        }
    }

    private val projectionLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == Activity.RESULT_OK && result.data != null) {
            val serviceIntent = Intent(this, ScreenCaptureService::class.java).apply {
                putExtra("resultCode", result.resultCode)
                putExtra("resultData", result.data)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                startForegroundService(serviceIntent)
            } else {
                startService(serviceIntent)
            }
            bindService(serviceIntent, serviceConnection, Context.BIND_AUTO_CREATE)
        } else {
            showSnackbar("用户取消或未授予截屏权限")
            updateUiState(false)
        }
    }

    private val serviceConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
            val binder = service as ScreenCaptureService.LocalBinder
            captureService = binder.getService()
            isBound = true
            startActiveStream()
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            captureService = null
            isBound = false
            updateUiState(false)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val (w, h) = getScreenMetrics()
        screenPhysicalW = w
        screenPhysicalH = h
        val ratio = w.toFloat() / h.toFloat()
        if (w < h) {
            targetWidth = 1080
            targetHeight = (((1080 / ratio).toInt() + 15) / 16) * 16
        } else {
            targetHeight = 1080
            targetWidth = (((1080 * ratio).toInt() + 15) / 16) * 16
        }

        val rootView = buildMaterial3Layout()
        setContentView(rootView)

        // 启动局域网发现，自动获取 PC 端 IP（供私有 UDP 通道使用）
        lanDiscovery = LanDiscovery(this).apply {
            onHostDiscovered = { host ->
                pcIp = host.ip
                runOnUiThread {
                    if (::statusTextView.isInitialized && !isStreamingActive) {
                        statusTextView.text = "已自动发现电脑端: ${host.ip}"
                    }
                }
            }
        }.also { it.startDiscovery() }

        ViewCompat.setOnApplyWindowInsetsListener(rootView) { v, insets ->
            val systemBars = insets.getInsets(WindowInsetsCompat.Type.systemBars())
            v.setPadding(systemBars.left, systemBars.top, systemBars.right, systemBars.bottom)
            insets
        }
    }

    private fun buildMaterial3Layout(): View {
        val rootLayout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
            val colorSurface = MaterialColors.getColor(this, com.google.android.material.R.attr.colorSurface)
            setBackgroundColor(colorSurface)
        }

        // 1. MD3 顶部工具栏
        val toolbar = MaterialToolbar(this).apply {
            title = "Castor NDI 极速投屏"
            subtitle = "广电级低延迟 NDI 发送端 (mDNS 零配置)"
            setTitleTextColor(MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnSurface))
            setSubtitleTextColor(MaterialColors.getColor(this, com.google.android.material.R.attr.colorOutline))
            elevation = dpToPx(2).toFloat()
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
        rootLayout.addView(toolbar)

        // 2. 滚动容器
        val scrollView = NestedScrollView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0, 1.0f
            )
        }

        val contentLayout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dpToPx(16), dpToPx(12), dpToPx(16), dpToPx(24))
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }

        // 卡片 1: NDI 局域网广播服务配置
        contentLayout.addView(buildNdiServiceCard())

        // 卡片 2: NDI 视频规格与调优
        contentLayout.addView(buildVideoConfigCard())

        // 卡片 3: 实时推流监控
        contentLayout.addView(buildStatusCard())

        scrollView.addView(contentLayout)
        rootLayout.addView(scrollView)

        return rootLayout
    }

    private fun buildNdiServiceCard(): MaterialCardView {
        return MaterialCardView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { setMargins(0, 0, 0, dpToPx(16)) }
            radius = dpToPx(16).toFloat()
            cardElevation = dpToPx(2).toFloat()
            strokeWidth = dpToPx(1)
            strokeColor = MaterialColors.getColor(this, com.google.android.material.R.attr.colorOutlineVariant)

            val inner = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dpToPx(16), dpToPx(16), dpToPx(16), dpToPx(16))
            }

            inner.addView(MaterialTextView(context).apply {
                text = "NDI 局域网广播服务"
                setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_TitleMedium)
                setTextColor(MaterialColors.getColor(this, androidx.appcompat.R.attr.colorPrimary))
            })

            inner.addView(MaterialTextView(context).apply {
                text = "● 协议标准: NewTek NDI 6\n● 零配置自动发现: 电脑端与手机在同一 Wi-Fi 下开播即可秒级识别，无需输入任何 IP 或端口。\n● 完美兼容苹果端【畅想 NDI】及电脑 Castor 接收器。"
                setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_BodySmall)
                setTextColor(MaterialColors.getColor(this, com.google.android.material.R.attr.colorOutline))
                setPadding(0, dpToPx(8), 0, dpToPx(12))
            })

            // NDI 源名称设置框
            val nameInputLayout = TextInputLayout(context).apply {
                hint = "NDI 广播源识别名称"
                boxBackgroundMode = TextInputLayout.BOX_BACKGROUND_OUTLINE
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                ).apply { setMargins(0, 0, 0, dpToPx(16)) }
            }
            ndiNameEditText = TextInputEditText(nameInputLayout.context).apply {
                setText("Castor-Android-Phone")
            }
            nameInputLayout.addView(ndiNameEditText)
            inner.addView(nameInputLayout)

            // 传输通道选择: NDI (兼容第三方) / 私有 UDP H.264 (低延迟高速)
            val modeRow = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
                ).apply { setMargins(0, 0, 0, dpToPx(12)) }
            }
            val btnNdiMode = MaterialButton(context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
                text = "通道: NDI (兼容)"
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { setMargins(0, 0, dpToPx(6), 0) }
                setOnClickListener {
                    useUdp = false
                    statusTextView.text = "已选通道: NDI (Full/SpeedHQ)"
                    if (!isStreamingActive) actionButton.text = "启动 NDI 屏幕广播"
                }
            }
            val btnUdpMode = MaterialButton(context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
                text = "通道: 私有UDP (低延迟)"
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                setOnClickListener {
                    useUdp = true
                    statusTextView.text = "已选通道: 私有 UDP H.264 (需电脑端已打开)"
                    if (!isStreamingActive) actionButton.text = "启动 私有UDP 推流"
                }
            }
            modeRow.addView(btnNdiMode)
            modeRow.addView(btnUdpMode)
            inner.addView(modeRow)

            // 主连接按钮
            actionButton = MaterialButton(context).apply {
                text = "启动 NDI 屏幕广播"
                cornerRadius = dpToPx(12)
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    dpToPx(52)
                )
                setOnClickListener {
                    if (!isStreamingActive) {
                        requestScreenCapture()
                    } else {
                        stopScreenCapture()
                    }
                }
            }
            inner.addView(actionButton)

            addView(inner)
        }
    }

    private fun buildVideoConfigCard(): MaterialCardView {
        return MaterialCardView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { setMargins(0, 0, 0, dpToPx(16)) }
            radius = dpToPx(16).toFloat()
            cardElevation = dpToPx(2).toFloat()
            strokeWidth = dpToPx(1)
            strokeColor = MaterialColors.getColor(this, com.google.android.material.R.attr.colorOutlineVariant)

            val inner = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dpToPx(16), dpToPx(16), dpToPx(16), dpToPx(16))
            }

            inner.addView(MaterialTextView(context).apply {
                text = "NDI 视频规格设置"
                setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_TitleMedium)
                setTextColor(MaterialColors.getColor(this, androidx.appcompat.R.attr.colorPrimary))
            })

            // 分辨率选择
            val resTitle = MaterialTextView(context).apply {
                text = "画面分辨率: 1080P 超清 ($targetWidth x $targetHeight)"
                setPadding(0, dpToPx(8), 0, dpToPx(4))
            }
            inner.addView(resTitle)

            val resLayout = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                )
            }
            val btn1080p = MaterialButton(context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
                text = "1080P 超清 (25ms)"
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f).apply {
                    setMargins(0, 0, dpToPx(6), 0)
                }
                setOnClickListener {
                    val (w, h) = getScreenMetrics()
                    val ratio = w.toFloat() / h.toFloat()
                    if (w < h) {
                        targetWidth = 1080
                        targetHeight = (((1080 / ratio).toInt() + 15) / 16) * 16
                    } else {
                        targetHeight = 1080
                        targetWidth = (((1080 * ratio).toInt() + 15) / 16) * 16
                    }
                    resTitle.text = "画面分辨率: 1080P 超清 ($targetWidth x $targetHeight)"
                    captureService?.updateResolution(targetWidth, targetHeight, resources.displayMetrics.densityDpi)
                }
            }
            val btn720p = MaterialButton(context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
                text = "720P 极速 (15ms)"
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f).apply {
                    setMargins(0, 0, dpToPx(6), 0)
                }
                setOnClickListener {
                    val (w, h) = getScreenMetrics()
                    val ratio = w.toFloat() / h.toFloat()
                    if (w < h) {
                        targetWidth = 720
                        targetHeight = (((720 / ratio).toInt() + 15) / 16) * 16
                    } else {
                        targetHeight = 720
                        targetWidth = (((720 * ratio).toInt() + 15) / 16) * 16
                    }
                    resTitle.text = "画面分辨率: 720P 极速 ($targetWidth x $targetHeight)"
                    captureService?.updateResolution(targetWidth, targetHeight, resources.displayMetrics.densityDpi)
                }
            }
            val btn540p = MaterialButton(context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
                text = "540P 极速"
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f).apply {
                    setMargins(0, 0, dpToPx(6), 0)
                }
                setOnClickListener {
                    val (w, h) = getScreenMetrics()
                    val ratio = w.toFloat() / h.toFloat()
                    if (w < h) {
                        targetWidth = 540
                        targetHeight = (((540 / ratio).toInt() + 15) / 16) * 16
                    } else {
                        targetHeight = 540
                        targetWidth = (((540 * ratio).toInt() + 15) / 16) * 16
                    }
                    resTitle.text = "画面分辨率: 540P 极速 ($targetWidth x $targetHeight)"
                    captureService?.updateResolution(targetWidth, targetHeight, resources.displayMetrics.densityDpi)
                }
            }
            val btnNative = MaterialButton(context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
                text = "1:1 原画"
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f)
                setOnClickListener {
                    val (w, h) = getScreenMetrics()
                    targetWidth = ((w / 16) * 16)
                    targetHeight = ((h / 16) * 16)
                    resTitle.text = "画面分辨率: 1:1 原画 ($targetWidth x $targetHeight)"
                    captureService?.updateResolution(targetWidth, targetHeight, resources.displayMetrics.densityDpi)
                }
            }
            resLayout.addView(btn1080p)
            resLayout.addView(btn720p)
            resLayout.addView(btn540p)
            resLayout.addView(btnNative)
            inner.addView(resLayout)

            // 帧率选择
            val fpsTitle = MaterialTextView(context).apply {
                text = "目标帧率: $targetFps FPS"
                setPadding(0, dpToPx(10), 0, dpToPx(4))
            }
            inner.addView(fpsTitle)

            val fpsLayout = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                )
            }
            for (fps in listOf(30, 60, 120)) {
                val btnFps = MaterialButton(context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
                    text = "${fps} FPS"
                    layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f).apply {
                        if (fps != 120) setMargins(0, 0, dpToPx(6), 0)
                    }
                    setOnClickListener {
                        targetFps = fps
                        fpsTitle.text = "目标帧率: $targetFps FPS"
                        captureService?.updateFps(fps)
                    }
                }
                fpsLayout.addView(btnFps)
            }
            inner.addView(fpsLayout)

            inner.addView(MaterialTextView(context).apply {
                text = "⚡ 底层集成官方 libndi.so 动态库 + NEON 指令集硬件加速，延迟稳定 20~30ms。"
                setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_BodySmall)
                setTextColor(MaterialColors.getColor(this, androidx.appcompat.R.attr.colorPrimary))
                setPadding(0, dpToPx(12), 0, 0)
            })

            addView(inner)
        }
    }

    private fun buildStatusCard(): MaterialCardView {
        return MaterialCardView(this).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
            radius = dpToPx(16).toFloat()
            cardElevation = dpToPx(2).toFloat()
            strokeWidth = dpToPx(1)
            strokeColor = MaterialColors.getColor(this, com.google.android.material.R.attr.colorOutlineVariant)

            val inner = LinearLayout(context).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dpToPx(16), dpToPx(16), dpToPx(16), dpToPx(16))
            }

            inner.addView(MaterialTextView(context).apply {
                text = "NDI 实时状态监控"
                setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_TitleMedium)
                setTextColor(MaterialColors.getColor(this, androidx.appcompat.R.attr.colorPrimary))
            })

            statusTextView = MaterialTextView(context).apply {
                text = "状态: 未广播 (就绪)"
                setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_BodyMedium)
                setPadding(0, dpToPx(6), 0, dpToPx(4))
            }
            inner.addView(statusTextView)

            liveStatsText = MaterialTextView(context).apply {
                text = "实时帧率: -- FPS | 广播带宽: -- Mbps"
                setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_BodyMedium)
                setPadding(0, 0, 0, dpToPx(6))
            }
            inner.addView(liveStatsText)

            inner.addView(MaterialButton(context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
                text = "① 实测 H.264 编码耗时 (隔离编码器≈25s)"
                cornerRadius = dpToPx(12)
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
                ).apply { setMargins(0, dpToPx(10), 0, 0) }
                setOnClickListener { requestH264Probe() }
            })

            addView(inner)
        }
    }

    // 按当前 targetWidth/targetHeight/useUdp 启动对应通道（首次连接、旋转重启都走这里）
    private fun startActiveStream() {
        val svc = captureService ?: return
        val streamName = ndiNameEditText.text?.toString()?.trim().takeUnless { it.isNullOrEmpty() } ?: "Castor-Android-Phone"
        val density = resources.displayMetrics.densityDpi

        if (pendingProbe) {
            pendingProbe = false
            svc.startH264Probe(
                width = targetWidth,
                height = targetHeight,
                fps = targetFps,
                densityDpi = density,
                onStatusChanged = { running, msg ->
                    runOnUiThread { updateUiState(running); statusTextView.text = msg }
                }
            )
            return
        }

        if (useUdp) {
            val ip = pcIp
            if (ip.isNullOrEmpty()) {
                statusTextView.text = "未发现电脑端 IP：请确认电脑已打开 Castor 且手机/电脑在同一 Wi-Fi"
                updateUiState(false)
                return
            }
            svc.startUdpStream(
                targetIp = ip,
                targetPort = 27190,
                width = targetWidth,
                height = targetHeight,
                fps = targetFps,
                bitrateMbps = 20,
                densityDpi = density,
                onStatusChanged = { running, msg ->
                    runOnUiThread { updateUiState(running); statusTextView.text = msg }
                },
                onStatsUpdated = { f, kbps ->
                    runOnUiThread {
                        liveStatsText.text = String.format("实时帧率: %.1f FPS | 带宽: %.1f Mbps", f, kbps / 1000.0f)
                    }
                }
            )
            return
        }

        svc.startStreaming(
            streamName = streamName,
            width = targetWidth,
            height = targetHeight,
            fps = targetFps,
            densityDpi = density,
            onStatusChanged = { running, msg ->
                runOnUiThread { updateUiState(running); statusTextView.text = msg }
            },
            onStatsUpdated = { fps, kbps ->
                runOnUiThread {
                    liveStatsText.text = String.format("实时帧率: %.1f FPS | 广播带宽: %.1f Mbps", fps, kbps / 1000.0f)
                }
            }
        )
    }

    private fun requestScreenCapture() {
        if (isStreamingActive) {
            stopScreenCapture()
            return
        }
        pendingProbe = false
        actionButton.isEnabled = false
        actionButton.text = "正在启动 NDI..."
        val projectionManager = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        projectionLauncher.launch(projectionManager.createScreenCaptureIntent())
    }

    /** 纯实测：跑一遍硬件 H.264 编码管线，量真实帧率/编码延迟/码率（不发网络）。结果见 logcat 标签 H264Probe。 */
    private fun requestH264Probe() {
        pendingProbe = true
        isStreamingActive = false
        actionButton.isEnabled = false
        actionButton.text = "正在启动编码实测..."
        val projectionManager = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        projectionLauncher.launch(projectionManager.createScreenCaptureIntent())
    }

    private fun stopScreenCapture() {
        actionButton.isEnabled = false
        if (isBound) {
            try {
                captureService?.stopStreaming()
                unbindService(serviceConnection)
            } catch (_: Exception) {}
            isBound = false
        }
        updateUiState(false)
        statusTextView.text = "状态: 未广播"
        liveStatsText.text = "实时帧率: -- FPS | 广播带宽: -- Mbps"
    }

    private fun updateUiState(running: Boolean) {
        isStreamingActive = running
        actionButton.isEnabled = true
        if (running) {
            actionButton.text = "停止 NDI 广播"
            actionButton.backgroundTintList = ColorStateList.valueOf(Color.parseColor("#E53935"))
        } else {
            actionButton.text = "启动 NDI 屏幕广播"
            val primaryColor = MaterialColors.getColor(actionButton, androidx.appcompat.R.attr.colorPrimary)
            actionButton.backgroundTintList = ColorStateList.valueOf(primaryColor)
        }
    }

    private fun showSnackbar(msg: String) {
        Snackbar.make(findViewById(android.R.id.content), msg, Snackbar.LENGTH_SHORT).show()
    }

    private fun dpToPx(dp: Int): Int {
        return TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP,
            dp.toFloat(),
            resources.displayMetrics
        ).toInt()
    }

    override fun onDestroy() {
        stopScreenCapture()
        try { lanDiscovery?.stopDiscovery() } catch (_: Exception) {}
        lanDiscovery = null
        super.onDestroy()
    }
}