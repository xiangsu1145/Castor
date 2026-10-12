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
    private var targetFps = 120         // 默认 120 帧高刷
    private var isStreamingActive = false
    private var pendingProbe = false
    private var useUdp = true           // 默认使用私有 UDP 超低延迟投屏
    private var pcIp: String? = null    // 自动发现的 PC IP
    private var lanDiscovery: LanDiscovery? = null
    private var ndiNameLayout: TextInputLayout? = null

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
            title = "Castor 无线投屏"
            subtitle = "电脑端自动连接"
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
                text = "投屏连接与传输"
                setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_TitleMedium)
                setTextColor(MaterialColors.getColor(this, androidx.appcompat.R.attr.colorPrimary))
            })

            inner.addView(MaterialTextView(context).apply {
                text = "手机和电脑连接在同一个 Wi-Fi 网络下即可投屏。\n● UDP 模式（默认）：专用于电脑端 Castor，延迟极低，画面清晰。\n● NDI 模式：通用兼容模式，可连接第三方 NDI 接收工具。"
                setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_BodySmall)
                setTextColor(MaterialColors.getColor(this, com.google.android.material.R.attr.colorOutline))
                setPadding(0, dpToPx(8), 0, dpToPx(12))
            })

            // NDI 源名称设置框（仅在 NDI 模式下展示）
            val nameInputLayout = TextInputLayout(context).apply {
                hint = "NDI 设备名称"
                boxBackgroundMode = TextInputLayout.BOX_BACKGROUND_OUTLINE
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                ).apply { setMargins(0, 0, 0, dpToPx(16)) }
                visibility = if (useUdp) View.GONE else View.VISIBLE
            }
            ndiNameLayout = nameInputLayout
            ndiNameEditText = TextInputEditText(nameInputLayout.context).apply {
                setText("Castor-Phone")
            }
            nameInputLayout.addView(ndiNameEditText)
            inner.addView(nameInputLayout)

            // 传输通道选择
            val modeRow = LinearLayout(context).apply {
                orientation = LinearLayout.HORIZONTAL
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
                ).apply { setMargins(0, 0, 0, dpToPx(12)) }
            }
            val btnUdpMode = MaterialButton(context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
                text = "UDP 模式 (推荐)"
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f).apply { setMargins(0, 0, dpToPx(6), 0) }
                setOnClickListener {
                    useUdp = true
                    ndiNameLayout?.visibility = View.GONE
                    statusTextView.text = if (pcIp.isNullOrEmpty()) "已切换到 UDP 模式，正在搜索电脑..." else "已选择 UDP 模式 (电脑: $pcIp)"
                    if (!isStreamingActive) actionButton.text = "启动无线投屏"
                }
            }
            val btnNdiMode = MaterialButton(context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
                text = "NDI 模式 (兼容)"
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                setOnClickListener {
                    useUdp = false
                    ndiNameLayout?.visibility = View.VISIBLE
                    statusTextView.text = "已切换到 NDI 模式"
                    if (!isStreamingActive) actionButton.text = "启动 NDI 广播"
                }
            }
            modeRow.addView(btnUdpMode)
            modeRow.addView(btnNdiMode)
            inner.addView(modeRow)

            // 主连接按钮
            actionButton = MaterialButton(context).apply {
                text = if (useUdp) "启动无线投屏" else "启动 NDI 广播"
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
                text = "画面规格设置"
                setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_TitleMedium)
                setTextColor(MaterialColors.getColor(this, androidx.appcompat.R.attr.colorPrimary))
            })

            // 分辨率选择
            val resTitle = MaterialTextView(context).apply {
                text = "分辨率: 1080P ($targetWidth x $targetHeight)"
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
                text = "1080P"
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
                    resTitle.text = "分辨率: 1080P ($targetWidth x $targetHeight)"
                    captureService?.updateResolution(targetWidth, targetHeight, resources.displayMetrics.densityDpi)
                }
            }
            val btn720p = MaterialButton(context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
                text = "720P"
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
                    resTitle.text = "分辨率: 720P ($targetWidth x $targetHeight)"
                    captureService?.updateResolution(targetWidth, targetHeight, resources.displayMetrics.densityDpi)
                }
            }
            val btn540p = MaterialButton(context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
                text = "540P"
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
                    resTitle.text = "分辨率: 540P ($targetWidth x $targetHeight)"
                    captureService?.updateResolution(targetWidth, targetHeight, resources.displayMetrics.densityDpi)
                }
            }
            val btnNative = MaterialButton(context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
                text = "原生原画"
                layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f)
                setOnClickListener {
                    val (w, h) = getScreenMetrics()
                    targetWidth = ((w / 16) * 16)
                    targetHeight = ((h / 16) * 16)
                    resTitle.text = "分辨率: 原生原画 ($targetWidth x $targetHeight)"
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
            for (fps in listOf(60, 90, 120, 144)) {
                val btnFps = MaterialButton(context, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
                    text = "${fps} 帧"
                    layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f).apply {
                        if (fps != 144) setMargins(0, 0, dpToPx(6), 0)
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
                text = "投屏状态"
                setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_TitleMedium)
                setTextColor(MaterialColors.getColor(this, androidx.appcompat.R.attr.colorPrimary))
            })

            statusTextView = MaterialTextView(context).apply {
                text = "状态: 未连接"
                setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_BodyMedium)
                setPadding(0, dpToPx(6), 0, dpToPx(4))
            }
            inner.addView(statusTextView)

            liveStatsText = MaterialTextView(context).apply {
                text = "实时帧率: -- FPS | 带宽: -- Mbps"
                setTextAppearance(com.google.android.material.R.style.TextAppearance_Material3_BodyMedium)
                setPadding(0, 0, 0, dpToPx(6))
            }
            inner.addView(liveStatsText)

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
        actionButton.text = if (useUdp) "正在连接..." else "正在启动..."
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
        statusTextView.text = "状态: 未连接"
        liveStatsText.text = "实时帧率: -- FPS | 带宽: -- Mbps"
    }

    private fun updateUiState(running: Boolean) {
        isStreamingActive = running
        actionButton.isEnabled = true
        if (running) {
            actionButton.text = if (useUdp) "停止无线投屏" else "停止 NDI 广播"
            actionButton.backgroundTintList = ColorStateList.valueOf(Color.parseColor("#E53935"))
        } else {
            actionButton.text = if (useUdp) "启动无线投屏" else "启动 NDI 广播"
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