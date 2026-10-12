package io.github.xiangsu1145.castor.stream

import java.nio.ByteBuffer
import java.nio.ByteOrder

object StreamProtocol {
    const val MAGIC: Int = 0x43415354 // "CAST" ASCII
    const val HEADER_SIZE: Int = 20

    const val TYPE_CONFIG: Byte = 0x01
    const val TYPE_VIDEO_FRAME: Byte = 0x02
    const val TYPE_HEARTBEAT: Byte = 0x03

    fun packHeader(type: Byte, seq: Int, payloadSize: Int, timestampNs: Long): ByteArray {
        val buf = ByteBuffer.allocate(HEADER_SIZE).order(ByteOrder.BIG_ENDIAN)
        buf.putInt(MAGIC)
        buf.put(type)
        buf.put(0.toByte()) // 保留字段
        buf.putShort(0.toShort()) // 保留字段
        buf.putInt(seq)
        buf.putInt(payloadSize)
        return buf.array()
    }
}
