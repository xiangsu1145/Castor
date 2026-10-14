package io.github.xiangsu1145.castor.stream

import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

/**
 * GpuFrameSplitter:
 * 基于 Android 单 VirtualDisplay 的 GPU 零拷贝画面分流器
 *
 * 核心原理：
 * 1. 避开 Android 14 严禁二次调用 createVirtualDisplay 的系统红线；
 * 2. 屏幕画面单一源头直入 GPU OES 纹理 (120 FPS 硬件直出)；
 * 3. 在 GPU 显存内将画面一分为二：
 *    - 全景/背景输出：绘制到全屏 MediaCodec Surface (发往 27190)；
 *    - 中心 1:1 原画输出：按物理像素无缩放裁剪中心 (如 640x640)，绘制到独立 MediaCodec Surface (发往 27192)；
 * 4. 彻底消除双流不同步问题（两路画面来自完全同一帧硬件时间戳）。
 */
class GpuFrameSplitter(
    private val fullSurface: Surface,
    private val fullWidth: Int,
    private val fullHeight: Int,
    private val roiSurface: Surface?,
    private val roiWidth: Int,
    private val roiHeight: Int
) {
    companion object {
        private const val TAG = "GpuFrameSplitter"
        private const val EGL_RECORDABLE_ANDROID = 0x3142

        private const val VERTEX_SHADER = """
            attribute vec4 aPosition;
            attribute vec2 aTexCoord;
            uniform mat4 uSTMatrix;
            varying vec2 vTexCoord;
            void main() {
                gl_Position = aPosition;
                vTexCoord = (uSTMatrix * vec4(aTexCoord, 0.0, 1.0)).xy;
            }
        """

        private const val FRAGMENT_SHADER = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            varying vec2 vTexCoord;
            uniform samplerExternalOES sTexture;
            void main() {
                gl_FragColor = texture2D(sTexture, vTexCoord);
            }
        """
    }

    private var glThread: HandlerThread? = null
    private var glHandler: Handler? = null

    private var eglDisplay: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var eglContext: EGLContext = EGL14.EGL_NO_CONTEXT
    private var eglConfig: EGLConfig? = null

    private var eglSurfaceFull: EGLSurface = EGL14.EGL_NO_SURFACE
    private var eglSurfaceRoi: EGLSurface = EGL14.EGL_NO_SURFACE

    private var oesTextureId = 0
    private var surfaceTexture: SurfaceTexture? = null
    private var inputSurface: Surface? = null

    private var program = 0
    private var aPositionLoc = 0
    private var aTexCoordLoc = 0
    private var uSTMatrixLoc = 0
    private var uSamplerLoc = 0

    private lateinit var fullVertexBuffer: FloatBuffer
    private lateinit var fullTexBuffer: FloatBuffer
    private lateinit var roiTexBuffer: FloatBuffer

    private val stMatrix = FloatArray(16)
    private val isRunning = AtomicBoolean(false)

    fun start(): Surface {
        val initLatch = CountDownLatch(1)
        glThread = HandlerThread("GpuFrameSplitterGL", Process.THREAD_PRIORITY_URGENT_DISPLAY).apply {
            start()
        }
        glHandler = Handler(glThread!!.looper)

        glHandler?.post {
            try {
                initEGL()
                initGL()
                isRunning.set(true)
            } catch (e: Exception) {
                Log.e(TAG, "EGL/GL 初始化失败: ${e.message}", e)
            } finally {
                initLatch.countDown()
            }
        }

        initLatch.await(3, TimeUnit.SECONDS)
        return inputSurface ?: throw IllegalStateException("未能创建 GpuFrameSplitter 输入 Surface")
    }

    private fun initEGL() {
        eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        if (eglDisplay == EGL14.EGL_NO_DISPLAY) {
            throw RuntimeException("eglGetDisplay 失败")
        }

        val version = IntArray(2)
        if (!EGL14.eglInitialize(eglDisplay, version, 0, version, 1)) {
            throw RuntimeException("eglInitialize 失败")
        }

        val attribList = intArrayOf(
            EGL14.EGL_RED_SIZE, 8,
            EGL14.EGL_GREEN_SIZE, 8,
            EGL14.EGL_BLUE_SIZE, 8,
            EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL_RECORDABLE_ANDROID, 1,
            EGL14.EGL_NONE
        )

        val configs = arrayOfNulls<EGLConfig>(1)
        val numConfigs = IntArray(1)
        if (!EGL14.eglChooseConfig(eglDisplay, attribList, 0, configs, 0, 1, numConfigs, 0) || numConfigs[0] <= 0) {
            throw RuntimeException("eglChooseConfig 失败")
        }
        eglConfig = configs[0]!!

        val contextAttribs = intArrayOf(
            EGL14.EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL14.EGL_NONE
        )
        eglContext = EGL14.eglCreateContext(eglDisplay, eglConfig, EGL14.EGL_NO_CONTEXT, contextAttribs, 0)
        if (eglContext == EGL14.EGL_NO_CONTEXT) {
            throw RuntimeException("eglCreateContext 失败")
        }

        val surfaceAttribs = intArrayOf(EGL14.EGL_NONE)
        eglSurfaceFull = EGL14.eglCreateWindowSurface(eglDisplay, eglConfig, fullSurface, surfaceAttribs, 0)
        if (eglSurfaceFull == EGL14.EGL_NO_SURFACE) {
            throw RuntimeException("eglCreateWindowSurface (Full) 失败")
        }

        if (roiSurface != null && roiWidth > 0 && roiHeight > 0) {
            eglSurfaceRoi = EGL14.eglCreateWindowSurface(eglDisplay, eglConfig, roiSurface, surfaceAttribs, 0)
            if (eglSurfaceRoi == EGL14.EGL_NO_SURFACE) {
                Log.w(TAG, "eglCreateWindowSurface (ROI) 失败，原画分流将不可用")
            }
        }

        if (!EGL14.eglMakeCurrent(eglDisplay, eglSurfaceFull, eglSurfaceFull, eglContext)) {
            throw RuntimeException("eglMakeCurrent 失败")
        }
    }

    private fun initGL() {
        val textures = IntArray(1)
        GLES20.glGenTextures(1, textures, 0)
        oesTextureId = textures[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTextureId)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)

        program = createProgram(VERTEX_SHADER, FRAGMENT_SHADER)
        aPositionLoc = GLES20.glGetAttribLocation(program, "aPosition")
        aTexCoordLoc = GLES20.glGetAttribLocation(program, "aTexCoord")
        uSTMatrixLoc = GLES20.glGetUniformLocation(program, "uSTMatrix")
        uSamplerLoc = GLES20.glGetUniformLocation(program, "sTexture")

        // 顶点坐标: 覆盖全视口 Quad (-1 ~ 1)
        val fullVertices = floatArrayOf(
            -1.0f, -1.0f,
             1.0f, -1.0f,
            -1.0f,  1.0f,
             1.0f,  1.0f
        )
        fullVertexBuffer = ByteBuffer.allocateDirect(fullVertices.size * 4)
            .order(ByteOrder.nativeOrder())
            .asFloatBuffer()
            .apply { put(fullVertices); position(0) }

        // 全景纹理坐标: (0 ~ 1)
        val fullTexCoords = floatArrayOf(
            0.0f, 0.0f,
            1.0f, 0.0f,
            0.0f, 1.0f,
            1.0f, 1.0f
        )
        fullTexBuffer = ByteBuffer.allocateDirect(fullTexCoords.size * 4)
            .order(ByteOrder.nativeOrder())
            .asFloatBuffer()
            .apply { put(fullTexCoords); position(0) }

        // 中心 1:1 原画裁剪纹理坐标: 计算物理像素在整屏中的百分比区间 [u0, u1], [v0, v1]
        val rw = minOf(roiWidth, fullWidth)
        val rh = minOf(roiHeight, fullHeight)
        val u0 = (fullWidth - rw) / (2.0f * fullWidth)
        val u1 = (fullWidth + rw) / (2.0f * fullWidth)
        val v0 = (fullHeight - rh) / (2.0f * fullHeight)
        val v1 = (fullHeight + rh) / (2.0f * fullHeight)

        val roiTexCoords = floatArrayOf(
            u0, v0,
            u1, v0,
            u0, v1,
            u1, v1
        )
        roiTexBuffer = ByteBuffer.allocateDirect(roiTexCoords.size * 4)
            .order(ByteOrder.nativeOrder())
            .asFloatBuffer()
            .apply { put(roiTexCoords); position(0) }

        surfaceTexture = SurfaceTexture(oesTextureId).apply {
            setDefaultBufferSize(fullWidth, fullHeight)
            setOnFrameAvailableListener({
                if (isRunning.get()) {
                    glHandler?.post { renderFrame() }
                }
            }, glHandler)
        }
        inputSurface = Surface(surfaceTexture)
        Log.i(TAG, "GpuFrameSplitter 已就绪: 全景 ${fullWidth}x${fullHeight}, 中心原画 ${roiWidth}x${roiHeight}")
    }

    private fun renderFrame() {
        val st = surfaceTexture ?: return
        try {
            st.updateTexImage()
            st.getTransformMatrix(stMatrix)
            val ptsNs = st.timestamp

            // 1. 渲染全景 / 背景流 (绘制到 eglSurfaceFull)
            EGL14.eglMakeCurrent(eglDisplay, eglSurfaceFull, eglSurfaceFull, eglContext)
            GLES20.glViewport(0, 0, fullWidth, fullHeight)
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)

            GLES20.glUseProgram(program)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTextureId)
            GLES20.glUniform1i(uSamplerLoc, 0)
            GLES20.glUniformMatrix4fv(uSTMatrixLoc, 1, false, stMatrix, 0)

            GLES20.glEnableVertexAttribArray(aPositionLoc)
            GLES20.glVertexAttribPointer(aPositionLoc, 2, GLES20.GL_FLOAT, false, 8, fullVertexBuffer)

            GLES20.glEnableVertexAttribArray(aTexCoordLoc)
            GLES20.glVertexAttribPointer(aTexCoordLoc, 2, GLES20.GL_FLOAT, false, 8, fullTexBuffer)

            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
            EGLExt.eglPresentationTimeANDROID(eglDisplay, eglSurfaceFull, ptsNs)
            EGL14.eglSwapBuffers(eglDisplay, eglSurfaceFull)

            // 2. 渲染中心 1:1 原画流 (使用中心裁剪纹理坐标绘制到 eglSurfaceRoi)
            if (eglSurfaceRoi != EGL14.EGL_NO_SURFACE && roiWidth > 0 && roiHeight > 0) {
                EGL14.eglMakeCurrent(eglDisplay, eglSurfaceRoi, eglSurfaceRoi, eglContext)
                GLES20.glViewport(0, 0, roiWidth, roiHeight)
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT)

                GLES20.glUseProgram(program)
                GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
                GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTextureId)
                GLES20.glUniform1i(uSamplerLoc, 0)
                GLES20.glUniformMatrix4fv(uSTMatrixLoc, 1, false, stMatrix, 0)

                GLES20.glEnableVertexAttribArray(aPositionLoc)
                GLES20.glVertexAttribPointer(aPositionLoc, 2, GLES20.GL_FLOAT, false, 8, fullVertexBuffer)

                GLES20.glEnableVertexAttribArray(aTexCoordLoc)
                GLES20.glVertexAttribPointer(aTexCoordLoc, 2, GLES20.GL_FLOAT, false, 8, roiTexBuffer)

                GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
                EGLExt.eglPresentationTimeANDROID(eglDisplay, eglSurfaceRoi, ptsNs)
                EGL14.eglSwapBuffers(eglDisplay, eglSurfaceRoi)
            }
        } catch (e: Exception) {
            Log.w(TAG, "渲染分流异常: ${e.message}")
        }
    }

    private fun createProgram(vertexCode: String, fragmentCode: String): Int {
        val vertexShader = loadShader(GLES20.GL_VERTEX_SHADER, vertexCode)
        val fragmentShader = loadShader(GLES20.GL_FRAGMENT_SHADER, fragmentCode)
        val prog = GLES20.glCreateProgram()
        GLES20.glAttachShader(prog, vertexShader)
        GLES20.glAttachShader(prog, fragmentShader)
        GLES20.glLinkProgram(prog)
        val linkStatus = IntArray(1)
        GLES20.glGetProgramiv(prog, GLES20.GL_LINK_STATUS, linkStatus, 0)
        if (linkStatus[0] != GLES20.GL_TRUE) {
            val log = GLES20.glGetProgramInfoLog(prog)
            GLES20.glDeleteProgram(prog)
            throw RuntimeException("Shader 链接失败: $log")
        }
        return prog
    }

    private fun loadShader(type: Int, code: String): Int {
        val shader = GLES20.glCreateShader(type)
        GLES20.glShaderSource(shader, code)
        GLES20.glCompileShader(shader)
        val compiled = IntArray(1)
        GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, compiled, 0)
        if (compiled[0] == 0) {
            val log = GLES20.glGetShaderInfoLog(shader)
            GLES20.glDeleteShader(shader)
            throw RuntimeException("Shader 编译失败 ($type): $log")
        }
        return shader
    }

    fun release() {
        isRunning.set(false)
        val latch = CountDownLatch(1)
        glHandler?.post {
            try {
                if (program != 0) {
                    GLES20.glDeleteProgram(program)
                    program = 0
                }
                if (oesTextureId != 0) {
                    GLES20.glDeleteTextures(1, intArrayOf(oesTextureId), 0)
                    oesTextureId = 0
                }
                surfaceTexture?.release()
                surfaceTexture = null
                inputSurface?.release()
                inputSurface = null

                if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
                    EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
                    if (eglSurfaceFull != EGL14.EGL_NO_SURFACE) {
                        EGL14.eglDestroySurface(eglDisplay, eglSurfaceFull)
                        eglSurfaceFull = EGL14.EGL_NO_SURFACE
                    }
                    if (eglSurfaceRoi != EGL14.EGL_NO_SURFACE) {
                        EGL14.eglDestroySurface(eglDisplay, eglSurfaceRoi)
                        eglSurfaceRoi = EGL14.EGL_NO_SURFACE
                    }
                    if (eglContext != EGL14.EGL_NO_CONTEXT) {
                        EGL14.eglDestroyContext(eglDisplay, eglContext)
                        eglContext = EGL14.EGL_NO_CONTEXT
                    }
                    EGL14.eglTerminate(eglDisplay)
                    eglDisplay = EGL14.EGL_NO_DISPLAY
                }
                Log.i(TAG, "GpuFrameSplitter 资源已全部释放")
            } catch (e: Exception) {
                Log.w(TAG, "释放 GpuFrameSplitter 异常: ${e.message}")
            } finally {
                latch.countDown()
            }
        }
        latch.await(1, TimeUnit.SECONDS)
        glThread?.quitSafely()
        glThread = null
        glHandler = null
    }
}
