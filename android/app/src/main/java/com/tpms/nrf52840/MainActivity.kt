package com.tpms.nrf52840

import android.Manifest
import android.animation.ObjectAnimator
import android.animation.ValueAnimator
import android.app.Dialog
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.graphics.Color
import android.graphics.drawable.ColorDrawable
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.SpannableString
import android.text.Spanned
import android.text.TextWatcher
import android.text.style.ForegroundColorSpan
import android.util.Log
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.Window
import android.widget.*
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import com.tpms.nrf52840.databinding.ActivityMainBinding
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.util.*

class MainActivity : AppCompatActivity() {

    companion object {
        val NUS_SERVICE_UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
        val NUS_RX_UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e")
        val NUS_TX_UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e")
        val CCCD_UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        const val TAG = "TPMS"
        const val REQUEST_PERMISSIONS = 1001
        const val STATUS_POLL_INTERVAL_MS = 4000L
        const val LONG_PRESS_MS = 2000L
    }

    private lateinit var binding: ActivityMainBinding
    private val handler = Handler(Looper.getMainLooper())

    private var bluetoothAdapter: BluetoothAdapter? = null
    private var bluetoothGatt: BluetoothGatt? = null
    private var scanner: BluetoothLeScanner? = null
    private var scanCallback: ScanCallback? = null

    private val foundDevices = mutableListOf<BluetoothDevice>()
    private var selectedDevice: BluetoothDevice? = null
    private var isConnected = false
    private var isAutoTx = false
    private var isNusReady = false
    private var pendingMessages = mutableListOf<String>()
    private var rxCharacteristic: BluetoothGattCharacteristic? = null
    private var cccdDescriptor: BluetoothGattDescriptor? = null
    private var cccdGatt: BluetoothGatt? = null
    private var cccdRetryCount = 0
    private val CCCD_MAX_RETRY = 3
    private val rxBuffer = StringBuilder()

    private val sensorViews = mutableListOf<SensorViewHolder>()
    private val sensorEnabled = booleanArrayOf(true, true, true, true)
    private val sensorNames = listOf(
        "Левое переднее", "Правое переднее", "Левое заднее", "Правое заднее"
    )

    private val sensorCardViews by lazy {
        listOf(
            binding.sensorFl.root, binding.sensorFr.root,
            binding.sensorRl.root, binding.sensorRr.root
        )
    }

    private var holdRunnable: Runnable? = null
    private var holdHandled = false
    private var editorDialog: Dialog? = null
    private var editorIndex = -1

    private val statusPollRunnable = object : Runnable {
        override fun run() {
            if (isConnected && isNusReady) {
                sendCommand("\"cmd\":\"status\"", quiet = true)
            }
            handler.postDelayed(this, STATUS_POLL_INTERVAL_MS)
        }
    }

    private var lastSniffSummary: String? = null
    private var sniffRunning = false

    private val snifferReport = mutableListOf<String>()
    private val lastDiscValid = booleanArrayOf(false, false, false, false)
    private var wasSniffActive = false

    private val prefs by lazy { getSharedPreferences("tpms_prefs", Context.MODE_PRIVATE) }

    private var deviceSerial = ""

    private fun updateLicenseUi(license: Int, trialRem: Long, serial: String) {
        if (serial.isNotEmpty()) deviceSerial = serial
        binding.licInputs.visibility = if (license == 2) View.GONE else View.VISIBLE
        when (license) {
            2 -> {
                binding.tvLicense.text = getString(R.string.license_passed)
                binding.tvLicense.setTextColor(getColor(R.color.accent))
            }
            1 -> {
                val h = trialRem / 3600.0
                val left = if (h >= 48) "%.0f дн".format(h / 24) else "%.1f ч".format(h)
                binding.tvLicense.text = getString(R.string.license_trial, left)
                binding.tvLicense.setTextColor(getColor(R.color.warning))
            }
            else -> {
                binding.tvLicense.text = getString(R.string.license_no_block)
                binding.tvLicense.setTextColor(getColor(R.color.danger))
            }
        }
    }

    private fun openLicenseEmail() {
        val subject = "Запрос лицензии TPMS-NRF52840"
        val body = buildString {
            appendLine("Добрый день!")
            appendLine()
            appendLine("Прошу выдать лицензионный ключ для устройства:")
            appendLine("Устройство: TPMS-NRF52840")
            if (deviceSerial.isNotEmpty()) {
                appendLine("ID устройства (serial): $deviceSerial")
            } else {
                appendLine("(ID устройства появится после подключения по Bluetooth — пришли письмо повторно)")
            }
            appendLine("Спасибо!")
        }
        val mailto = Uri.fromParts("mailto", "wargaelanor@ya.ru", null).toString()
        val intent = Intent(Intent.ACTION_SENDTO).apply {
            data = Uri.parse(mailto)
            putExtra(Intent.EXTRA_EMAIL, arrayOf("wargaelanor@ya.ru"))
            putExtra(Intent.EXTRA_SUBJECT, subject)
            putExtra(Intent.EXTRA_TEXT, body)
        }
        runCatching { startActivity(Intent.createChooser(intent, "Отправить запрос лицензии")) }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        val bm = getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        bluetoothAdapter = bm.adapter
        scanner = bluetoothAdapter?.bluetoothLeScanner

        initSensorsUi()
        initButtons()
        initTabs()
        startDotPulse()
        checkPermissions()
        tryAutoConnect()
        handler.postDelayed(statusPollRunnable, STATUS_POLL_INTERVAL_MS)
    }

    private fun startDotPulse() {
        val anim = ObjectAnimator.ofFloat(binding.statusDot, View.ALPHA, 1f, 0.35f)
        anim.duration = 1250
        anim.repeatMode = ValueAnimator.REVERSE
        anim.repeatCount = ValueAnimator.INFINITE
        anim.start()
    }

    private fun initTabs() {
        binding.btnTabMain.setOnClickListener { selectTab(0) }
        binding.btnTabSettings.setOnClickListener { selectTab(1) }
        selectTab(0)
    }

    private fun selectTab(pos: Int) {
        val main = pos == 0
        binding.scrollMain.visibility = if (main) View.VISIBLE else View.GONE
        binding.scrollSettings.visibility = if (main) View.GONE else View.VISIBLE
        if (!main) binding.scrollSettings.post { binding.scrollSettings.scrollTo(0, 0) }

        binding.btnTabMain.background = ContextCompat.getDrawable(this,
            if (main) R.drawable.bg_tab_active else R.drawable.bg_tab)
        binding.btnTabSettings.background = ContextCompat.getDrawable(this,
            if (main) R.drawable.bg_tab else R.drawable.bg_tab_active)
        binding.btnTabMain.setTextColor(getColor(if (main) R.color.accent else R.color.text_secondary))
        binding.btnTabSettings.setTextColor(getColor(if (main) R.color.text_secondary else R.color.accent))
    }

    private fun tryAutoConnect() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED
        ) return
        val bonded = bluetoothAdapter?.bondedDevices ?: emptySet()
        val device = bonded.firstOrNull {
            val n = it.name ?: ""
            n.contains("TPMS", true) || n.contains("NRF", true)
        }
        if (device != null) {
            Log.d(TAG, "Auto-connect to bonded ${device.name} ${device.address}")
            log("Автоподключение к ${device.name}...")
            selectedDevice = device
            connect(device)
        } else {
            Log.d(TAG, "No bonded TPMS device found, starting scan")
            log("Поиск устройства...")
            startScan()
        }
    }

    private fun initSensorsUi() {
        sensorViews.clear()
        for (i in 0 until 4) {
            val holder = SensorViewHolder(sensorCardViews[i], i)
            holder.tvTitle.text = sensorNames[i].uppercase()
            attachSensorInteractions(holder)
            sensorViews.add(holder)
        }
    }

    private fun attachSensorInteractions(holder: SensorViewHolder) {
        holder.root.setOnTouchListener { v, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    holdHandled = false
                    v.isPressed = true
                    holdRunnable?.let { v.removeCallbacks(it) }
                    val r = Runnable {
                        holdHandled = true
                        v.isPressed = false
                        v.performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
                        openSensorEditor(holder.index)
                    }
                    holdRunnable = r
                    v.postDelayed(r, LONG_PRESS_MS)
                }
                MotionEvent.ACTION_CANCEL -> {
                    holdRunnable?.let { v.removeCallbacks(it) }
                    holdRunnable = null
                    v.isPressed = false
                }
                MotionEvent.ACTION_UP -> {
                    holdRunnable?.let { v.removeCallbacks(it) }
                    holdRunnable = null
                    v.isPressed = false
                    v.performClick()
                    if (!holdHandled) {
                        // Короткий тап = имитация передачи (аналог TX)
                        sendCommand("\"cmd\":\"tx${holder.index + 1}\"")
                        flashCard(v)
                    }
                }
            }
            true
        }
    }

    private fun flashCard(v: View) {
        v.animate().scaleX(1.08f).scaleY(1.08f).setDuration(110)
            .withEndAction {
                v.animate().scaleX(1f).scaleY(1f).setDuration(110).start()
            }.start()
    }

    private fun openSensorEditor(idx: Int) {
        val h = sensorViews[idx]
        editorIndex = idx
        val v = LayoutInflater.from(this).inflate(R.layout.sensor_editor, null)
        val tvTitle = v.findViewById<TextView>(R.id.tvEditorTitle)
        val etId = v.findViewById<EditText>(R.id.etEditorId)
        val etPr = v.findViewById<EditText>(R.id.etEditorPressure)
        val etTmp = v.findViewById<EditText>(R.id.etEditorTemp)
        val cbEn = v.findViewById<CheckBox>(R.id.cbEditorEnabled)

        tvTitle.text = sensorNames[idx]
        etId.setText(h.tvId.text)
        etPr.setText(if (h.tvPressure.text == "--") "" else h.tvPressure.text)
        etTmp.setText(if (h.tvTemp.text == "--") "" else h.tvTemp.text)
        cbEn.isChecked = sensorEnabled[idx]

        v.findViewById<View>(R.id.btnEditorClose).setOnClickListener { dismissEditor() }
        v.findViewById<View>(R.id.btnEditorCancel).setOnClickListener { dismissEditor() }
        v.findViewById<View>(R.id.btnEditorSave).setOnClickListener {
            val id = etId.text.toString().trim()
            val pressure = etPr.text.toString().toIntOrNull() ?: 230
            val temp = etTmp.text.toString().toIntOrNull() ?: 20
            val enabled = cbEn.isChecked
            h.tvId.text = id
            h.tvPressure.text = pressure.toString()
            h.tvTemp.text = temp.toString()
            sendCommand("\"cmd\":\"sensor\",\"sensor\":$idx,\"id\":\"$id\",\"pressure\":$pressure,\"temp\":$temp,\"enabled\":$enabled")
            dismissEditor()
        }

        val dialog = Dialog(this)
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE)
        dialog.setContentView(v)
        dialog.window?.apply {
            setGravity(Gravity.BOTTOM)
            setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
            setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            setWindowAnimations(R.style.DialogBottomAnim)
        }
        dialog.setCanceledOnTouchOutside(true)
        dialog.setOnDismissListener { editorIndex = -1 }
        dialog.show()
        editorDialog = dialog
    }

    private fun dismissEditor() {
        editorDialog?.dismiss()
        editorDialog = null
        editorIndex = -1
    }

    private fun initButtons() {
        binding.btnBurst.setOnClickListener { sendCommand("\"cmd\":\"burst\"") }
        binding.btnSaveAutoTx.setOnClickListener {
            val interval = binding.etInterval.text.toString().toIntOrNull() ?: 360
            val packets = binding.etPackets.text.toString().toIntOrNull() ?: 2
            sendCommand("\"cmd\":\"autotx\",\"interval\":$interval,\"packets\":$packets")
        }
        binding.btnSniffToggle.setOnClickListener {
            sniffRunning = !sniffRunning
            binding.btnSniffToggle.text =
                getString(if (sniffRunning) R.string.button_sniff_stop else R.string.button_sniff_start)
            sendCommand(if (sniffRunning) "\"cmd\":\"sniff_start\"" else "\"cmd\":\"sniff_stop\"")
            if (sniffRunning) {
                snifferReport.clear()
                lastDiscValid.fill(false)
                wasSniffActive = false
            }
            refreshSnifferStatus()
        }
        binding.btnSniffApply.setOnClickListener {
            sendCommand("\"cmd\":\"sniff_apply\"")
            val valid = lastDiscValid.count { it }
            snifferReport.add(getString(R.string.sniff_applied, valid))
            refreshSnifferStatus()
        }
        binding.btnSaveFreq.setOnClickListener {
            val freq = if (binding.rb433.isChecked) 433 else 315
            sendCommand("\"cmd\":\"settings\",\"freq\":$freq")
        }
        binding.btnActivate.setOnClickListener {
            val key = binding.etLicense.text.toString().trim()
            if (key.length == 8) {
                sendCommand("\"cmd\":\"license\",\"key\":\"$key\"")
            } else {
                log(getString(R.string.cmd_key_length))
            }
        }
        binding.btnLicenseEmail.setOnClickListener { openLicenseEmail() }
        binding.btnBattCal.setOnClickListener {
            val mv = binding.etBattCalMv.text.toString().toIntOrNull()
            if (mv != null && mv > 0) {
                sendCommand("\"cmd\":\"battcal\",\"mv\":$mv")
                prefs.edit().putLong("batt_calib_time", System.currentTimeMillis()).apply()
                updateBattCalibLine()
            } else {
                log(getString(R.string.cmd_cal_invalid))
            }
        }
        binding.btnReset.setOnClickListener { sendCommand("\"cmd\":\"battcal_reset\"") }
        binding.etCapacity.setText(prefs.getInt("batt_capacity_mah", 850).toString())
        attachPressFlash(
            binding.btnBurst, binding.btnSaveAutoTx, binding.btnSniffToggle,
            binding.btnSniffApply, binding.btnSaveFreq, binding.btnActivate,
            binding.btnBattCal, binding.btnReset,
            binding.btnTabMain, binding.btnTabSettings
        )

        val battWatcher = object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
            override fun afterTextChanged(s: Editable?) { refreshBatteryLife() }
        }
        listOf(binding.etCapacity, binding.etInterval, binding.etPackets).forEach { it.addTextChangedListener(battWatcher) }
        refreshBatteryLife()
    }

    private fun attachPressFlash(vararg views: View) {
        views.forEach { v ->
            v.setOnTouchListener { vv, e ->
                when (e.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        vv.animate().scaleX(0.94f).scaleY(0.94f).setDuration(90).start()
                    }
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        vv.animate().scaleX(1f).scaleY(1f).setDuration(130).start()
                    }
                }
                false
            }
        }
    }

    private fun refreshBatteryLife() {
        val capacity = binding.etCapacity.text.toString().toIntOrNull() ?: 0
        val interval = binding.etInterval.text.toString().toIntOrNull() ?: 0
        val packets = binding.etPackets.text.toString().toIntOrNull() ?: 0
        if (capacity <= 0 || interval <= 0 || packets < 1) {
            binding.tvBattLifeResult.text = getString(R.string.batt_life_hint_res)
            binding.tvBattLifeResult.setTextColor(getColor(R.color.danger))
            return
        }
        prefs.edit().putInt("batt_capacity_mah", capacity).apply()

        // Режим сна: между передачами BLE выключен, CC1101 обесточен, CPU спит.
        val sleepMa = 0.05
        // Энергия одного burst (мА·с): пробуждение+инициализация CC1101,
        // пакеты TX (25 мА ~60 мс) и паузы между ними (3 мА ~100 мс).
        val wakeMaS = 15.0 * 0.03
        val pktMaS = 25.0 * 0.06
        val gapMaS = 3.0 * 0.1
        val burstMaS = wakeMaS + packets * pktMaS + (packets - 1) * gapMaS
        val avgMa = sleepMa + burstMaS / interval
        val hours = capacity / avgMa

        binding.tvBattLifeResult.text = getString(R.string.batt_life_res, formatDuration(hours))
        binding.tvBattLifeResult.setTextColor(getColor(R.color.accent))
    }

    private fun formatDuration(hours: Double): String = when {
        hours >= 8760 -> "%.1f лет".format(hours / 8760)
        hours >= 720 -> "%.1f мес".format(hours / 720)
        hours >= 48 -> "%d дн %d ч".format((hours / 24).toInt(), hours.toInt() % 24)
        hours >= 1 -> "%.1f ч".format(hours)
        else -> "%.0f мин".format(hours * 60)
    }

    private fun formatAgo(timeMillis: Long): String {
        if (timeMillis <= 0) return "нет"
        val diff = System.currentTimeMillis() - timeMillis
        val h = diff / 3600000.0
        val d = diff / 86400000.0
        return when {
            diff < 60000 -> "только что"
            d >= 2 -> "%.0f дн назад".format(d)
            d >= 1 -> "%.1f дн назад".format(d)
            h >= 1 -> "%.1f ч назад".format(h)
            else -> "%.0f мин назад".format(diff / 60000.0)
        }
    }

    private fun updateBattCalibLine() {
        val calibTime = prefs.getLong("batt_calib_time", 0L)
        val text = binding.tvBattDetails.text?.toString() ?: ""
        val prefix = text.substringBefore("Последняя калибровка:")
        binding.tvBattDetails.text = prefix + "Последняя калибровка: ${formatAgo(calibTime)}"
    }

    private fun checkPermissions() {
        val permissions = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            permissions.add(Manifest.permission.BLUETOOTH_SCAN)
            permissions.add(Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            permissions.add(Manifest.permission.BLUETOOTH)
            permissions.add(Manifest.permission.BLUETOOTH_ADMIN)
            permissions.add(Manifest.permission.ACCESS_FINE_LOCATION)
        }
        val needed = permissions.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (needed.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, needed.toTypedArray(), REQUEST_PERMISSIONS)
        }
    }

    private fun startScan() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED
        ) {
            log("Нет разрешения на сканирование")
            return
        }
        foundDevices.clear()
        selectedDevice = null
        binding.tvConnectionState.text = getString(R.string.scanning)
        binding.tvConnectionState.setTextColor(getColor(R.color.accent_text))

        scanCallback?.let { scanner?.stopScan(it) }
        scanCallback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult?) {
                Log.d(TAG, "onScanResult: ${result?.device?.address} name=${result?.scanRecord?.deviceName}")
                result?.device?.let { onDeviceFound(it, result.scanRecord?.deviceName) }
            }

            override fun onBatchScanResults(results: MutableList<ScanResult>?) {
                Log.d(TAG, "onBatchScanResults: ${results?.size}")
                results?.forEach { onDeviceFound(it.device, it.scanRecord?.deviceName) }
            }

            override fun onScanFailed(errorCode: Int) {
                Log.e(TAG, "onScanFailed: $errorCode")
                runOnUiThread {
                    log("Scan failed: $errorCode")
                    binding.tvConnectionState.text = "Ошибка сканирования: $errorCode"
                    binding.tvConnectionState.setTextColor(getColor(R.color.danger))
                }
            }
        }
        Log.d(TAG, "startScan")
        scanner?.startScan(null, ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), scanCallback)

        handler.postDelayed({ stopScan() }, 10000)
    }

    private fun stopScan() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED
        ) return
        scanCallback?.let { scanner?.stopScan(it) }
        scanCallback = null
        showDevicePicker()
    }

    private fun onDeviceFound(device: BluetoothDevice, name: String?) {
        if (foundDevices.any { it.address == device.address }) return
        val n = name ?: device.name
        if (!n.isNullOrBlank() && (n.contains("TPMS", true) || n.contains("NRF", true))) {
            foundDevices.add(device)
        }
    }

    private fun showDevicePicker() {
        if (foundDevices.isEmpty()) {
            val bonded = bluetoothAdapter?.bondedDevices ?: emptySet()
            bonded.filter {
                val n = it.name ?: ""
                n.contains("TPMS", true) || n.contains("NRF", true)
            }.forEach { foundDevices.add(it) }
        }
        val items = foundDevices.map { "${it.name ?: "Unknown"} (${it.address})" }.toTypedArray()
        if (items.isEmpty()) {
            binding.tvConnectionState.text = getString(R.string.no_devices)
            binding.tvConnectionState.setTextColor(getColor(R.color.danger))
            return
        }
        AlertDialog.Builder(this)
            .setTitle(getString(R.string.btn_select))
            .setItems(items) { _, which ->
                selectedDevice = foundDevices[which]
                binding.tvConnectionState.text = "Выбрано: ${foundDevices[which].name}"
                binding.tvConnectionState.setTextColor(getColor(R.color.accent_text))
                connect(foundDevices[which])
            }
            .setNegativeButton(getString(R.string.button_cancel), null)
            .show()
    }

    private fun connect(device: BluetoothDevice) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED
        ) {
            log("Нет разрешения на подключение")
            return
        }
        disconnect()
        binding.tvConnectionState.text = getString(R.string.connecting)
        binding.tvConnectionState.setTextColor(getColor(R.color.accent_text))
        bluetoothGatt = device.connectGatt(this, false, gattCallback)
    }

    private fun disconnect() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED
        ) return
        bluetoothGatt?.close()
        bluetoothGatt = null
        rxCharacteristic = null
        isConnected = false
        isNusReady = false
        pendingMessages.clear()
        rxBuffer.clear()
        updateConnectionState()
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt?, status: Int, newState: Int) {
            super.onConnectionStateChange(gatt, status, newState)
            Log.d(TAG, "onConnectionStateChange status=$status newState=$newState")
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                runOnUiThread {
                    binding.tvConnectionState.text = getString(R.string.connecting_services)
                    binding.tvConnectionState.setTextColor(getColor(R.color.accent_text))
                    log("BLE подключен")
                }
                handler.postDelayed({
                    try {
                        val method = gatt?.javaClass?.getMethod("refresh")
                        method?.invoke(gatt)
                    } catch (_: Exception) {}
                    handler.postDelayed({
                        gatt?.discoverServices()
                    }, 100)
                }, 200)
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                rxCharacteristic = null
                isNusReady = false
                pendingMessages.clear()
                rxBuffer.clear()
                gatt?.close()
                if (gatt == bluetoothGatt) bluetoothGatt = null
                runOnUiThread {
                    isConnected = false
                    updateConnectionState()
                    log("BLE отключен, переподключение...")
                }
                handler.postDelayed({ tryAutoConnect() }, 3000)
            }
        }

        override fun onMtuChanged(gatt: BluetoothGatt?, mtu: Int, status: Int) {
            super.onMtuChanged(gatt, mtu, status)
            Log.d(TAG, "onMtuChanged mtu=$mtu status=$status")
            gatt?.discoverServices()
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt?, status: Int) {
            super.onServicesDiscovered(gatt, status)
            Log.d(TAG, "onServicesDiscovered status=$status")
            if (status != BluetoothGatt.GATT_SUCCESS) {
                runOnUiThread { log("Ошибка обнаружения сервисов: $status") }
                return
            }
            val service = gatt?.getService(NUS_SERVICE_UUID)
            if (service == null) {
                runOnUiThread { log("NUS сервис не найден") }
                return
            }
            val txChar = service.getCharacteristic(NUS_TX_UUID)
            val rxChar = service.getCharacteristic(NUS_RX_UUID)
            if (txChar == null || rxChar == null) {
                runOnUiThread { log("NUS характеристики не найдены") }
                return
            }
            rxCharacteristic = rxChar
            Log.d(TAG, "tx props=${txChar.properties} rx props=${rxChar.properties}")

            handler.postDelayed({
                gatt.setCharacteristicNotification(txChar, true)
                val descriptor = txChar.getDescriptor(CCCD_UUID)
                Log.d(TAG, "CCCD descriptor: $descriptor")
                if (descriptor != null) {
                    cccdDescriptor = descriptor
                    cccdGatt = gatt
                    cccdRetryCount = 0
                    val result = descriptor.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                    Log.d(TAG, "descriptor.setValue result=$result")
                    val writeResult = gatt.writeDescriptor(descriptor)
                    Log.d(TAG, "writeDescriptor result=$writeResult")
                } else {
                    Log.d(TAG, "CCCD descriptor not found, calling onNusReady directly")
                    onNusReady()
                }
            }, 200)

            runOnUiThread {
                isConnected = true
                updateConnectionState()
                log("NUS сервис найден")
            }
        }

        override fun onCharacteristicChanged(gatt: BluetoothGatt?, characteristic: BluetoothGattCharacteristic?) {
            super.onCharacteristicChanged(gatt, characteristic)
            if (characteristic?.uuid == NUS_TX_UUID) {
                val bytes = characteristic.value ?: byteArrayOf()
                val value = String(bytes, Charsets.UTF_8)
                Log.d(TAG, "RX fragment (${bytes.size}): $value")
                rxBuffer.append(value)
                var nlIndex = rxBuffer.indexOf("\n")
                while (nlIndex >= 0) {
                    val line = rxBuffer.substring(0, nlIndex)
                    rxBuffer.delete(0, nlIndex + 1)
                    if (line.isNotBlank()) {
                        Log.d(TAG, "RX line: $line")
                        runOnUiThread { onNusData(line) }
                    }
                    nlIndex = rxBuffer.indexOf("\n")
                }
            }
        }

        override fun onDescriptorWrite(gatt: BluetoothGatt?, descriptor: BluetoothGattDescriptor?, status: Int) {
            super.onDescriptorWrite(gatt, descriptor, status)
            Log.d(TAG, "onDescriptorWrite status=$status")
            if (status == BluetoothGatt.GATT_SUCCESS) {
                cccdDescriptor = null
                cccdGatt = null
                cccdRetryCount = 0
                onNusReady()
            } else if (cccdRetryCount < CCCD_MAX_RETRY) {
                cccdRetryCount++
                Log.d(TAG, "CCCD write failed, retry $cccdRetryCount/$CCCD_MAX_RETRY")
                val desc = cccdDescriptor
                val g = cccdGatt
                if (desc != null && g != null) {
                    handler.postDelayed({
                        desc.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                        g.writeDescriptor(desc)
                    }, 200)
                } else {
                    runOnUiThread { log("Ошибка включения уведомлений: $status") }
                }
            } else {
                runOnUiThread { log("Ошибка включения уведомлений: $status (после $CCCD_MAX_RETRY попыток)") }
            }
        }

        override fun onCharacteristicWrite(gatt: BluetoothGatt?, characteristic: BluetoothGattCharacteristic?, status: Int) {
            super.onCharacteristicWrite(gatt, characteristic, status)
            Log.d(TAG, "onCharacteristicWrite status=$status uuid=${characteristic?.uuid}")
            if (status != BluetoothGatt.GATT_SUCCESS) {
                runOnUiThread { log("Ошибка записи: $status") }
                isSendingChunks = false
                pendingChunks.clear()
            } else if (isSendingChunks && pendingChunks.isNotEmpty()) {
                sendNextChunk()
            } else {
                isSendingChunks = false
                sendNextPending()
            }
        }
    }

    private fun onNusReady() {
        if (isNusReady) return
        isNusReady = true
        runOnUiThread { log("NUS готов к передаче") }
        sendCommand("\"cmd\":\"status\"")
        while (pendingMessages.isNotEmpty()) {
            sendCommand(pendingMessages.removeAt(0).removeSurrounding("{", "}"))
        }
    }

    private fun sendNextPending() {
        if (pendingMessages.isEmpty()) return
        val next = pendingMessages.removeAt(0)
        sendCommandInternal(next)
    }

    private val pendingChunks = mutableListOf<Byte>()
    private var isSendingChunks = false

    private fun sendCommandInternal(payload: String, quiet: Boolean = false) {
        val msg = "{$payload}\n"
        val rx = rxCharacteristic
        if (rx == null || bluetoothGatt == null) {
            Log.w(TAG, "sendCommandInternal: not ready")
            return
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED
        ) {
            Log.w(TAG, "sendCommandInternal: no BLUETOOTH_CONNECT permission")
            return
        }
        val bytes = msg.toByteArray(Charsets.UTF_8)
        val chunkSize = 20
        if (bytes.size <= chunkSize) {
            rx.value = bytes
            val ok = bluetoothGatt?.writeCharacteristic(rx) ?: false
            Log.d(TAG, "TX: $msg ok=$ok")
            if (!quiet) runOnUiThread { log("→ $msg") }
        } else {
            pendingChunks.clear()
            pendingChunks.addAll(bytes.toList())
            isSendingChunks = false
            Log.d(TAG, "TX chunked (${bytes.size} bytes in ${(bytes.size + chunkSize - 1) / chunkSize} chunks): $msg")
            if (!quiet) runOnUiThread { log("→ $msg (${bytes.size}b)") }
            sendNextChunk()
        }
    }

    private fun sendNextChunk() {
        if (pendingChunks.isEmpty()) {
            isSendingChunks = false
            return
        }
        val rx = rxCharacteristic ?: return
        val chunkSize = 20
        val chunk = pendingChunks.take(chunkSize).toByteArray()
        pendingChunks.subList(0, minOf(chunkSize, pendingChunks.size)).clear()
        isSendingChunks = true
        rx.value = chunk
        bluetoothGatt?.writeCharacteristic(rx)
    }

    private fun sendCommand(payload: String, quiet: Boolean = false) {
        if (!isConnected || bluetoothGatt == null) {
            if (!quiet) log("Не подключено")
            return
        }
        if (!isNusReady) {
            if (!quiet) {
                pendingMessages.add(payload)
                log("В очереди: $payload")
            }
            return
        }
        if (pendingMessages.isNotEmpty()) {
            if (!quiet) pendingMessages.add(payload)
            return
        }
        sendCommandInternal(payload, quiet)
    }

    private fun onNusData(data: String) {
        try {
            val json = JSONObject(data)
            when (json.optString("t")) {
                "status" -> json.optJSONObject("data")?.let { applyStatus(it) }
                "log" -> log(json.optString("m"))
                "pkt" -> {
                    val s = json.optInt("s", -1)
                    val id = json.optString("id", "?")
                    val p = json.optInt("p", 0)
                    val tmp = json.optInt("tmp", 0)
                    val c = json.optInt("c", 0)
                    val crc = json.optString("crc", "?")
                    log("TX PKT #${s}: id=$id p=$p t=$tmp cnt=$c crc=$crc")
                }
                "disc" -> {
                    val id = json.optString("id", "?")
                    val p = json.optInt("p", 0)
                    val tmp = json.optInt("tmp", 0)
                    val rssi = json.optInt("rssi", 0)
                    log("DISC: id=$id p=$p t=$tmp rssi=$rssi")
                }
                else -> log(data)
            }
        } catch (e: Exception) {
            log(data)
        }
    }

    private fun applyStatus(data: JSONObject) {
        val sensors = data.optJSONArray("sensors")
        sensors?.let {
            for (i in 0 until minOf(it.length(), sensorViews.size)) {
                val s = it.getJSONObject(i)
                val h = sensorViews[i]
                h.tvId.text = s.optString("id", "00000000")
                h.tvPressure.text = s.optInt("p", 0).toString()
                h.tvTemp.text = s.optInt("t", 0).toString()
                sensorEnabled[i] = s.optInt("en", 1) == 1
            }
        }

        val freq = data.optInt("freq", 315)
        binding.rb315.isChecked = freq == 315
        binding.rb433.isChecked = freq == 433

        isAutoTx = data.optInt("tx_en", 0) == 1
        if (!binding.etInterval.hasFocus()) binding.etInterval.setText(data.optInt("tx_int", 360).toString())
        if (!binding.etPackets.hasFocus()) binding.etPackets.setText(data.optInt("tx_pkt", 2).toString())

        updateLicenseUi(data.optInt("license", 0), data.optLong("trial_rem", 0), data.optString("serial", ""))

        val mv = data.optInt("batt_mv", 0)
        val pct = data.optInt("batt_pct", 0)
        val raw = data.optInt("batt_raw", 0)
        val calMv = data.optInt("batt_cal", 0)
        val usb = data.optInt("usb", 0) == 1

        val usbMode = usb || (mv == 0 && raw > 0)
        binding.batteryIcon.setPercent(if (usbMode) -1 else pct)
        binding.tvBattDetails.text = buildString {
            append("Напряжение: ${"%.2f".format(mv / 1000.0)} В ($pct%)\n")
            if (usb) append("Питание: USB (АКБ не измеряется)\n")
            append("Последняя калибровка: ${formatAgo(prefs.getLong("batt_calib_time", 0L))}")
        }
        if (calMv > 0 && !binding.etBattCalMv.hasFocus()) binding.etBattCalMv.setText(calMv.toString())

        val sniffActive = data.optInt("sniff", 0) == 1
        if (sniffRunning != sniffActive) {
            sniffRunning = sniffActive
            binding.btnSniffToggle.text =
                getString(if (sniffRunning) R.string.button_sniff_stop else R.string.button_sniff_start)
        }
        val disc = data.optJSONArray("disc")
        updateSnifferUi(sniffActive, disc)
        val sb = StringBuilder()
        sb.appendLine("Сниффер: ${if (sniffActive) "активен" else "выкл"}")
        disc?.let { a ->
            for (i in 0 until a.length()) {
                val obj = a.optJSONObject(i)
                if (obj != null) {
                    sb.appendLine("  #${i}: id=${obj.optString("id")} p=${obj.optInt("p")} t=${obj.optInt("t")} rssi=${obj.optInt("rssi")}")
                }
            }
        }
        val summary = sb.toString().trimEnd()
        if (summary != lastSniffSummary) {
            lastSniffSummary = summary
            log(summary)
        }
    }

    private fun updateSnifferUi(sniffActive: Boolean, disc: JSONArray?) {
        disc?.let { a ->
            for (i in 0 until a.length()) {
                val obj = a.optJSONObject(i)
                if (obj != null) {
                    val id = obj.optString("id", "?")
                    if (!lastDiscValid[i] && id != "?") {
                        lastDiscValid[i] = true
                        snifferReport.add(getString(R.string.sniff_found, i + 1, id))
                    }
                }
            }
        }
        val allValid = lastDiscValid.all { it }
        if (wasSniffActive && !sniffActive && allValid) {
            snifferReport.add(getString(R.string.sniff_all_found))
        }
        wasSniffActive = sniffActive
        refreshSnifferStatus()
    }

    private fun refreshSnifferStatus() {
        val sb = StringBuilder(
            getString(if (sniffRunning) R.string.sniffer_on else R.string.sniffer_off))
        if (snifferReport.isNotEmpty()) {
            sb.append("\n")
            sb.append(snifferReport.joinToString("\n"))
        }
        binding.tvSnifferStatus.text = sb.toString()
        binding.tvSnifferStatus.setTextColor(getColor(
            if (sniffRunning) R.color.accent_text else R.color.text_muted))
    }

    private fun updateConnectionState() {
        if (isConnected) {
            binding.tvConnectionState.text = getString(R.string.connected,
                selectedDevice?.name ?: selectedDevice?.address ?: "?")
            binding.tvConnectionState.setTextColor(getColor(R.color.accent))
        } else {
            binding.tvConnectionState.text = getString(R.string.searching)
            binding.tvConnectionState.setTextColor(getColor(R.color.warning))
        }
    }

    private fun log(msg: String) {
        val tv = binding.tvLog
        val color = when {
            msg.startsWith("→") -> getColor(R.color.warning)
            msg.startsWith("DISC:") || msg.startsWith("TX PKT") -> getColor(R.color.accent)
            else -> getColor(R.color.accent_text)
        }
        val s = SpannableString(msg)
        s.setSpan(ForegroundColorSpan(color), 0, s.length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        tv.append(s)
        tv.append("\n")
        val scroll = tv.parent as? ScrollView
        val outer = binding.scrollSettings
        if (scroll != null && outer != null) {
            outer.post {
                val ptOut = IntArray(2)
                val ptLog = IntArray(2)
                outer.getLocationInWindow(ptOut)
                tv.getLocationInWindow(ptLog)
                val relY = ptLog[1] - ptOut[1]
                if (relY >= 0 && relY < outer.height) {
                    val range = (scroll.getChildAt(0)?.height ?: 0) - scroll.height
                    val atBottom = range <= 0 || scroll.scrollY >= range - 32
                    if (atBottom) scroll.fullScroll(View.FOCUS_DOWN)
                }
            }
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQUEST_PERMISSIONS) {
            if (grantResults.any { it != PackageManager.PERMISSION_GRANTED }) {
                Toast.makeText(this, "Без разрешений Bluetooth работать не будет", Toast.LENGTH_LONG).show()
            } else {
                tryAutoConnect()
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        disconnect()
    }

    class SensorViewHolder(view: View, val index: Int) {
        val root: View = view
        val tvTitle: TextView = view.findViewById(R.id.tvCardTitle)
        val tvId: TextView = view.findViewById(R.id.tvCardId)
        val tvPressure: TextView = view.findViewById(R.id.tvCardPressure)
        val tvTemp: TextView = view.findViewById(R.id.tvCardTemp)
    }
}