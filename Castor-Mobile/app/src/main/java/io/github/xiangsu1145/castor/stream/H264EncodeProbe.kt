package io.github.xiangsu1145.castor.stream

import android.graphics.Canvas
import android.graphics.Color
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.media.projection.MediaProjection
import android.os.SystemClock
import android.util.Log
import android.view.Surface
import java.util.ArrayDeque
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 全量探针（一次跑完，避开 Android「每个 MediaProjection 只能建一个 VirtualDisplay」的限制）：
 *   Trial 1 MANUAL : 手动向编码器 Surface 喂帧（不经 VirtualDisplay/合成器）→ 隔离「编码器本身」延迟，用 FIFO 配对测量
 *   Trial 2 VD     : VirtualDisplay 抓真实屏幕 → 编码器（最佳配置：LATENCY=0, 无REPEAT, OPERATING_RATE, 高通低延迟开关），用采集PTS测量
 * 对比 1 vs 2 就能知道：32ms 到底花在编码器上，还是花在采集/合成管线上。
 * 每组记录：帧率 / 码率 / 延迟 avg-p50-min-max / 延迟直方图 / 相邻PTS间隔。不发网络数据。标签 H264Probe。
 */
class H264EncodeProbe(
    private val mediaProjection: MediaProjection,
    private val width: Int = 1080,
    private val height: Int = 1536,
    private val fps: Int = 60,
    private val bitrateMbps: Int = 18,
    private val densityDpi: Int = 400,
    private val trialSeconds: Int = 10
) {
    companion object {
        private const val TAG = "H264Probe"
        private const val MIME = MediaFormat.MIMETYPE_VIDEO_AVC
        private const val VENDOR_LL = "vendor.qti-ext-enc-low-latency.enable"
        private val BINS = intArrayOf(0, 8, 16, 24, 32, 48, 64, 1000)
    }

    private val running = AtomicBoolean(false)
    private var thread: Thread? = null
    var onStatus: ((Boolean, String) -> Unit)? = null

    fun start() {
        if (running.getAndSet(true)) return
        thread = Thread({
            try {
                logEncoderInfo()
                runManualTrial()
                runVdTrial()
                Log.i(TAG, "======== 全部实测完成 ========")
            } catch (e: Exception) {
                Log.e(TAG, "探针异常: ${e.message}", e)
            } finally {
                running.set(false)
                onStatus?.invoke(false, "实测完成")
            }
        }, "H264Probe").apply { start() }
    }

    private fun logEncoderInfo() {
        try {
            val list = MediaCodecList(MediaCodecList.REGULAR_CODECS)
            for (info in list.codecInfos) {
                if (info.isEncoder && info.supportedTypes.any { it.equals(MIME, true) }) {
                    Log.i(TAG, "编码器: ${info.name} | 硬编=${info.isHardwareAccelerated} | vendor=${info.isVendor}")
                }
            }
        } catch (e: Exception) { Log.w(TAG, "列举编码器失败: ${e.message}") }
    }

    private fun buildFormat(aw: Int, ah: Int, repeatPrevUs: Long, operatingRateFps: Int, vendor: Boolean, logKeys: Boolean): MediaFormat {
        val fmt = MediaFormat.createVideoFormat(MIME, aw, ah)
        fun si(k: String, v: Int) { try { fmt.setInteger(k, v); if (logKeys) Log.i(TAG, "  $k=$v 已设") } catch (e: Exception) { Log.w(TAG, "  $k=$v 失败: ${e.message}") } }
        fun sl(k: String, v: Long) { try { fmt.setLong(k, v); if (logKeys) Log.i(TAG, "  $k=$v 已设") } catch (e: Exception) { Log.w(TAG, "  $k=$v 失败: ${e.message}") } }
        si(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
        si(MediaFormat.KEY_BIT_RATE, bitrateMbps * 1_000_000)
        si(MediaFormat.KEY_FRAME_RATE, fps)
        si(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
        si(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
        si(MediaFormat.KEY_LATENCY, 0)
        si(MediaFormat.KEY_PRIORITY, 0)
        if (operatingRateFps > 0) si(MediaFormat.KEY_OPERATING_RATE, operatingRateFps)
        if (repeatPrevUs >= 0) sl(MediaFormat.KEY_REPEAT_PREVIOUS_FRAME_AFTER, repeatPrevUs)
        if (vendor) si(VENDOR_LL, 1)
        return fmt
    }

    // ---------- Trial 1: 手动喂帧，隔离编码器 ----------
    private fun runManualTrial() {
        if (!running.get()) return
        val aw = ((width + 15) / 16) * 16
        val ah = ((height + 15) / 16) * 16
        val name = "1_MANUAL_isolate_encoder"
        Log.i(TAG, "======== 试验[$name] ${aw}x${ah}@${fps} ${bitrateMbps}Mbps LAT0+OpRate+vendor 无VD ========")

        var codec: MediaCodec? = null
        var feeder: Thread? = null
        val feederStop = AtomicBoolean(false)
        try {
            codec = MediaCodec.createEncoderByType(MIME)
            codec.configure(buildFormat(aw, ah, -1L, fps, true, true), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            val surface: Surface = codec.createInputSurface()
            codec.start()

            val submitQueue = ArrayDeque<Long>()
            feeder = Thread({
                val colors = intArrayOf(Color.RED, Color.GREEN, Color.BLUE, Color.YELLOW, Color.CYAN, Color.MAGENTA)
                var i = 0
                val t0 = SystemClock.elapsedRealtimeNanos()
                while (running.get() && !feederStop.get()) {
                    try {
                        val c: Canvas = try { surface.lockHardwareCanvas() } catch (_: Throwable) { surface.lockCanvas(null) }
                        c.drawColor(colors[i % colors.size])
                        submitQueue.addLast(System.nanoTime() / 1000) // 提交时刻(µs)
                        surface.unlockCanvasAndPost(c)
                    } catch (e: Exception) { Log.w(TAG, "喂帧异常: ${e.message}"); break }
                    i++
                    val target = t0 + i * 1_000_000_000L / fps
                    val sl = (target - SystemClock.elapsedRealtimeNanos()) / 1_000_000L
                    if (sl > 0) try { Thread.sleep(sl) } catch (_: InterruptedException) {}
                }
            }, "H264Feeder").apply { start() }

            collect(codec, name, submitQueue)

        } catch (e: Exception) {
            Log.e(TAG, "试验[$name] 异常: ${e.message}", e)
        } finally {
            feederStop.set(true)
            try { feeder?.interrupt() } catch (_: Exception) {}
            try { codec?.stop() } catch (_: Exception) {}
            try { codec?.release() } catch (_: Exception) {}
        }
    }

    // ---------- Trial 2: VirtualDisplay（唯一一次） ----------
    private fun runVdTrial() {
        if (!running.get()) return
        val aw = ((width + 15) / 16) * 16
        val ah = ((height + 15) / 16) * 16
        val name = "2_VD_capture_best_cfg"
        Log.i(TAG, "======== 试验[$name] ${aw}x${ah}@${fps} ${bitrateMbps}Mbps LAT0+OpRate+vendor 无REPEAT ========")

        var codec: MediaCodec? = null
        var vd: VirtualDisplay? = null
        try {
            codec = MediaCodec.createEncoderByType(MIME)
            codec.configure(buildFormat(aw, ah, -1L, fps, true, false), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            val surface = codec.createInputSurface()
            codec.start()
            vd = mediaProjection.createVirtualDisplay("CastorProbe", aw, ah, densityDpi,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, surface, null, null)
            collect(codec, name, null)
        } catch (e: Exception) {
            Log.e(TAG, "试验[$name] 异常: ${e.message}", e)
        } finally {
            try { vd?.release() } catch (_: Exception) {}
            try { codec?.stop() } catch (_: Exception) {}
            try { codec?.release() } catch (_: Exception) {}
        }
    }

    // ---------- 采集 ----------
    private fun collect(codec: MediaCodec, name: String, submitQueue: ArrayDeque<Long>?) {
        val info = MediaCodec.BufferInfo()
        val hist = IntArray(BINS.size - 1)
        var frames = 0; var bytes = 0L
        var latSum = 0L; var latMin = Long.MAX_VALUE; var latMax = 0L; var latCount = 0
        val lats = ArrayList<Long>(4096)
        var ptsSum = 0L; var ptsCount = 0; var lastPts = -1L
        var winStart = SystemClock.elapsedRealtime(); val hardStart = winStart

        while (running.get()) {
            val idx = codec.dequeueOutputBuffer(info, 10000)
            if (idx >= 0) {
                if (info.size > 0) {
                    frames++; bytes += info.size
                    val nowUs = System.nanoTime() / 1000
                    // 手动喂帧：用 FIFO 提交时刻配对；VD：用采集 PTS
                    val refUs = if (submitQueue != null) (submitQueue.pollFirst() ?: -1L) else info.presentationTimeUs
                    val lat = nowUs - refUs
                    if (refUs > 0 && lat in 0..2_000_000L) {
                        lats.add(lat); latSum += lat; latCount++
                        if (lat < latMin) latMin = lat
                        if (lat > latMax) latMax = lat
                        for (b in 0 until BINS.size - 1) if (lat >= BINS[b] * 1000L && lat < BINS[b + 1] * 1000L) { hist[b]++; break }
                    }
                    if (lastPts > 0 && info.presentationTimeUs > lastPts) { ptsSum += info.presentationTimeUs - lastPts; ptsCount++ }
                    lastPts = info.presentationTimeUs
                }
                codec.releaseOutputBuffer(idx, false)
            } else if (idx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                Log.i(TAG, "  实际 outputFormat: ${codec.outputFormat}")
            }
            val now = SystemClock.elapsedRealtime(); val dt = now - winStart
            if (dt >= 1000) {
                Log.i(TAG, String.format("  [%s] 输出=%.1f fps | 延迟 avg=%.1fms | 码率=%.1f Mbps",
                    name, frames * 1000.0 / dt, if (latCount > 0) latSum / latCount / 1000.0 else -1.0, bytes * 8.0 / (dt / 1000.0) / 1e6))
                frames = 0; bytes = 0; winStart = now
            }
            if (now - hardStart >= trialSeconds * 1000L) break
        }

        lats.sort()
        val p50 = if (lats.isNotEmpty()) lats[lats.size / 2] / 1000.0 else -1.0
        val hs = StringBuilder()
        for (b in 0 until BINS.size - 1) hs.append("${BINS[b]}-${BINS[b + 1]}=${hist[b]} ")
        Log.i(TAG, "======== 试验[$name] 小结: 样本=$latCount | 延迟 min=%.1f avg=%.1f p50=%.1f max=%.1f ms".format(
            if (latCount > 0) latMin / 1000.0 else -1.0, if (latCount > 0) latSum.toDouble() / latCount / 1000.0 else -1.0, p50, latMax / 1000.0))
        Log.i(TAG, "  直方图(ms): $hs")
        Log.i(TAG, "  相邻PTS间隔 avg=${if (ptsCount > 0) ptsSum.toDouble() / ptsCount / 1000.0 else -1.0}ms")
        Log.i(TAG, "======== 试验[$name] 结束 ========")
    }

    fun stop() {
        if (!running.getAndSet(false)) return
        try { thread?.interrupt() } catch (_: Exception) {}
        thread = null
        onStatus?.invoke(false, "实测已停止")
    }
}
