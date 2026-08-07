package com.tpms.nrf52840

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.util.AttributeSet
import android.view.View

class BatteryView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

    private val outlinePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = dp(2.5f)
        color = 0xFF4ADE80.toInt()
    }

    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.FILL
        color = 0xFF4ADE80.toInt()
    }

    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = 0xFFE0F2E9.toInt()
        typeface = android.graphics.Typeface.DEFAULT_BOLD
        textAlign = Paint.Align.CENTER
    }

    private var percent = -1
    private var label = ""
    private val fillRect = RectF()
    private val bodyRect = RectF()

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val w = width.toFloat()
        val h = height.toFloat()
        val capW = w * 0.06f
        val capH = h * 0.34f

        canvas.drawRoundRect(
            RectF(w - capW, (h - capH) / 2f, w, (h + capH) / 2f),
            dp(1.5f), dp(1.5f), outlinePaint
        )

        bodyRect.set(0f, 0f, w - capW, h)
        canvas.drawRoundRect(bodyRect, h / 5f, h / 5f, outlinePaint)

        if (percent >= 0) {
            val inset = outlinePaint.strokeWidth + dp(1.5f)
            val left = inset
            val top = inset
            val right = w - capW - inset
            val bottom = h - inset
            val fillWidth = (right - left) * (percent.coerceIn(0, 100) / 100f)
            if (fillWidth > 0f) {
                fillRect.set(left, top, left + fillWidth, bottom)
                canvas.drawRoundRect(fillRect, h / 5f - dp(1f), h / 5f - dp(1f), fillPaint)
            }
        }

        if (label.isNotEmpty()) {
            textPaint.textSize = h * 0.42f
            val cx = (w - capW) / 2f
            val cy = h / 2f - (textPaint.ascent() + textPaint.descent()) / 2f
            canvas.drawText(label, cx, cy, textPaint)
        }
    }

    fun setPercent(pct: Int) {
        percent = pct
        fillPaint.color = when {
            pct < 0 -> 0x00000000
            pct < 10 -> 0xFFFCA5A5.toInt()
            pct < 30 -> 0xFFFBBD24.toInt()
            else -> 0xFF4ADE80.toInt()
        }
        label = if (pct < 0) "USB" else "$pct"
        invalidate()
    }

    private fun dp(v: Float): Float = v * resources.displayMetrics.density
}