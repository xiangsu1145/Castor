package io.github.xiangsu1145.castor.stream

import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * UDP 视频极速发送器：将每帧 JPEG 拆分为 1200B 的分片直接 sendto 发送。
 * PC 端接收到任一分片即重组，丢帧即跳过，绝不产生网络排队延迟。
 */
class UdpSender(host: String, private val port: Int) {

    companion object {
        private const val TAG = "UdpSender"
        private const val FRAG_MAGIC = 0x31554253 // "SBU1"
        private const val PING_MAGIC = 0x474E4950 // "PING"
        private const val PONG_MAGIC = 0x474E4F50 // "PONG"
        private const val FRAG_HEADER = 24
        private const val FRAG_PAYLOAD = 1400
        private const val STAT_INTERVAL_MS = 2000L
    }

    private val socket = DatagramSocket().apply {
        sendBufferSize = 1024 * 1024
    }
    private val addr: InetAddress = InetAddress.getByName(host)

    private val buf = ByteArray(FRAG_HEADER + FRAG_PAYLOAD)
    private val bb = ByteBuffer.wrap(buf).order(ByteOrder.LITTLE_ENDIAN)
    private val packet = DatagramPacket(buf, buf.size, addr, port)

    private var frameId = 0
    private var statFrames = 0
    private var statBytes = 0L
    private var lastStat = System.currentTimeMillis()

    @Volatile private var isRunning = true
    private val echoThread = Thread {
        val echoBuf = ByteArray(64)
        val echoPacket = DatagramPacket(echoBuf, echoBuf.size)
        while (isRunning) {
            try {
                socket.receive(echoPacket)
                if (echoPacket.length >= 16) {
                    val magic = ByteBuffer.wrap(echoPacket.data, 0, 4).order(ByteOrder.LITTLE_ENDIAN).int
                    if (magic == PING_MAGIC) {
                        // 将 Magic 变为 PONG，原样反射回给 PC 发包源端口
                        ByteBuffer.wrap(echoPacket.data, 0, 4).order(ByteOrder.LITTLE_ENDIAN).putInt(PONG_MAGIC)
                        socket.send(echoPacket)
                    }
                }
            } catch (e: Exception) {
                if (!isRunning) break
            }
        }
    }.apply {
        name = "Castor-EchoThread"
        isDaemon = true
        start()
    }

    var onStatsUpdated: ((fps: Float, bitrateMbps: Float) -> Unit)? = null

    fun sendFrame(jpeg: ByteArray, ptsUs: Long) {
        val fragCount = (jpeg.size + FRAG_PAYLOAD - 1) / FRAG_PAYLOAD
        for (i in 0 until fragCount) {
            val off = i * FRAG_PAYLOAD
            val len = minOf(FRAG_PAYLOAD, jpeg.size - off)
            bb.clear()
            bb.putInt(FRAG_MAGIC)
            bb.putInt(frameId)
            bb.putShort(i.toShort())
            bb.putShort(fragCount.toShort())
            bb.putLong(ptsUs)
            bb.putShort(len.toShort())
            bb.putShort(0)
            System.arraycopy(jpeg, off, buf, FRAG_HEADER, len)
            packet.length = FRAG_HEADER + len
            socket.send(packet)
            statBytes += FRAG_HEADER + len
        }
        statFrames++
        frameId++

        val now = System.currentTimeMillis()
        if (now - lastStat >= STAT_INTERVAL_MS) {
            val secs = (now - lastStat) / 1000.0f
            val fps = statFrames / secs
            val mbps = (statBytes * 8.0f) / (secs * 1_000_000.0f)
            onStatsUpdated?.invoke(fps, mbps)
            lastStat = now
            statFrames = 0
            statBytes = 0
        }
    }

    fun close() {
        isRunning = false
        try {
            socket.close()
        } catch (_: Exception) {}
        try {
            echoThread.interrupt()
        } catch (_: Exception) {}
    }
}
