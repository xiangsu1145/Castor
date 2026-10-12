package io.github.xiangsu1145.castor.net

import android.content.Context
import android.net.wifi.WifiManager
import android.util.Log
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.SocketTimeoutException
import java.util.concurrent.atomic.AtomicBoolean

data class DiscoveredHost(
    val name: String,
    val ip: String,
    val port: Int,
    val lastSeenMs: Long = System.currentTimeMillis()
)

class LanDiscovery(private val context: Context) {
    companion object {
        const val DISCOVERY_PORT = 19889
        const val MAGIC_PROBE = "CASTOR_DISCOVERY_PROBE"
        const val MAGIC_PREFIX = "CASTOR_BEACON:"
        private const val TAG = "LanDiscovery"
    }

    private val isRunning = AtomicBoolean(false)
    private var workerThread: Thread? = null
    private var multicastLock: WifiManager.MulticastLock? = null

    var onHostDiscovered: ((DiscoveredHost) -> Unit)? = null

    fun startDiscovery() {
        if (isRunning.getAndSet(true)) return

        acquireMulticastLock()

        workerThread = Thread {
            var socket: DatagramSocket? = null
            try {
                socket = DatagramSocket(null).apply {
                    reuseAddress = true
                    bind(java.net.InetSocketAddress(DISCOVERY_PORT))
                    broadcast = true
                    soTimeout = 1500
                }
                Log.d(TAG, "UDP 局域网扫描服务已启动, 监听端口: $DISCOVERY_PORT")

                val buffer = ByteArray(1024)
                val packet = DatagramPacket(buffer, buffer.size)

                while (isRunning.get()) {
                    // 1. 每隔两秒主动向局域网广播查询 (包含全局 255.255.255.255 与具体子网广播)
                    val phoneSentUs = android.os.SystemClock.elapsedRealtimeNanos() / 1000
                    try {
                        val probeStr = "$MAGIC_PROBE:$phoneSentUs"
                        val probeData = probeStr.toByteArray()
                        val targets = getBroadcastTargets()
                        for (target in targets) {
                            try {
                                val probePacket = DatagramPacket(probeData, probeData.size, target, DISCOVERY_PORT)
                                socket.send(probePacket)
                            } catch (_: Exception) {}
                        }
                    } catch (e: Exception) {
                        Log.w(TAG, "广播发送探测失败: ${e.message}")
                    }

                    // 2. 接收响应或心跳
                    try {
                        socket.receive(packet)
                        val text = String(packet.data, 0, packet.length).trim()
                        if (text.startsWith(MAGIC_PREFIX)) {
                            // 格式: CASTOR_BEACON:PC_NAME:IP:PORT 或 CASTOR_BEACON:PC_NAME:IP:PORT:PHONE_US:PC_US
                            val parts = text.split(":")
                            if (parts.size >= 4) {
                                val name = parts[1]
                                val ip = parts[2]
                                val port = parts[3].toIntOrNull() ?: 19888
                                val host = DiscoveredHost(name, ip, port)
                                onHostDiscovered?.invoke(host)

                                if (parts.size >= 6) {
                                    val echoPhoneUs = parts[4].toLongOrNull() ?: 0L
                                    val pcUs = parts[5].toLongOrNull() ?: 0L
                                    val phoneRecvUs = android.os.SystemClock.elapsedRealtimeNanos() / 1000
                                    val rtt = phoneRecvUs - echoPhoneUs
                                    if (echoPhoneUs > 0 && rtt in 1..200_000) {
                                        val offset = (echoPhoneUs + phoneRecvUs) / 2 - pcUs
                                        ClockSync.updateOffset(offset)
                                    }
                                }
                            }
                        }
                    } catch (e: SocketTimeoutException) {
                        // 正常超时轮询
                    } catch (e: Exception) {
                        if (isRunning.get()) {
                            Log.w(TAG, "接收广播异常: ${e.message}")
                        }
                    }

                    try {
                        Thread.sleep(500)
                    } catch (_: InterruptedException) {
                        break
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "UDP 端口绑定失败: ${e.message}")
            } finally {
                socket?.close()
                releaseMulticastLock()
            }
        }.apply {
            name = "Castor-LanDiscovery"
            isDaemon = true
            start()
        }
    }

    fun stopDiscovery() {
        isRunning.set(false)
        workerThread?.interrupt()
        workerThread = null
    }

    private fun acquireMulticastLock() {
        try {
            val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
            multicastLock = wifi?.createMulticastLock("CastorDiscoveryLock")?.apply {
                setReferenceCounted(true)
                acquire()
            }
        } catch (e: Exception) {
            Log.w(TAG, "获取 MulticastLock 失败: ${e.message}")
        }
    }

    private fun releaseMulticastLock() {
        try {
            multicastLock?.let {
                if (it.isHeld) it.release()
            }
        } catch (_: Exception) {}
        multicastLock = null
    }

    private fun getBroadcastTargets(): List<InetAddress> {
        val targets = mutableListOf<InetAddress>()
        try {
            targets.add(InetAddress.getByName("255.255.255.255"))
        } catch (_: Exception) {}

        try {
            val interfaces = java.net.NetworkInterface.getNetworkInterfaces()
            while (interfaces.hasMoreElements()) {
                val ni = interfaces.nextElement()
                if (ni.isLoopback || !ni.isUp) continue
                for (ia in ni.interfaceAddresses) {
                    val b = ia.broadcast
                    if (b != null && !targets.contains(b)) {
                        targets.add(b)
                    }
                }
            }
        } catch (_: Exception) {}
        return targets
    }
}
