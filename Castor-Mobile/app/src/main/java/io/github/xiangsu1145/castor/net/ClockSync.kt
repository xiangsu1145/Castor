package io.github.xiangsu1145.castor.net

/**
 * 手机与 PC 时钟差同步单例 (NTP 原理):
 * 用于消除手机系统时钟与 PC steady_clock 的基准偏差，
 * 计算出毫秒级精准的端到端真实传输延迟。
 */
object ClockSync {
    @Volatile var offsetUs: Long = 0L
    @Volatile var isSynced: Boolean = false

    fun updateOffset(offset: Long) {
        // 使用一阶滤波平滑时钟差
        if (!isSynced) {
            offsetUs = offset
            isSynced = true
        } else {
            offsetUs = (offsetUs * 0.8f + offset * 0.2f).toLong()
        }
    }

    fun toPcTimeUs(phoneUs: Long): Long {
        return if (isSynced) phoneUs - offsetUs else phoneUs
    }
}
