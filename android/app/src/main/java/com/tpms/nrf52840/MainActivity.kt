package com.tpms.nrf52840

import android.Manifest
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.LayoutInflater
import android.view.View
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

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        val bm = getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        bluetoothAdapter = bm.adapter
        scanner = bluetoothAdapter?.bluetoothLeScanner

        initSensorsUi()
        initButtons()
        checkPermissions()
        tryAutoConnect()
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
            Log.d(TAG, "No bonded TPMS device found")
        }
    }

    private fun initSensorsUi() {
        val positions = listOf("Левое Переднее", "Правое Переднее", "Левое Заднее", "Правое Заднее")
        val container = binding.sensorsContainer
        container.removeAllViews()
        sensorViews.clear()
        for (i in 0 until 4) {
            val v = LayoutInflater.from(this).inflate(R.layout.item_sensor, container, false)
            val holder = SensorViewHolder(v, i)
            holder.tvPosition.text = positions[i]
            holder.btnSave.setOnClickListener { sendSensor(i) }
            holder.btnTx.setOnClickListener { sendCommand("\"cmd\":\"tx${i + 1}\"") }
            container.addView(v)
            sensorViews.add(holder)
        }
    }

    private fun initButtons() {
        binding.btnScan.setOnClickListener { startScan() }
        binding.btnConnect.setOnClickListener {
            if (isConnected) disconnect()
            else selectedDevice?.let { connect(it) }
        }
        binding.btnBurst.setOnClickListener { sendCommand("\"cmd\":\"burst\"") }
        binding.btnAutoTx.setOnClickListener {
            val cmd = if (isAutoTx) "\"cmd\":\"stop\"" else "\"cmd\":\"tx\""
            sendCommand(cmd)
        }
        binding.btnSaveAutoTx.setOnClickListener {
            val interval = binding.etInterval.text.toString().toIntOrNull() ?: 300
            val packets = binding.etPackets.text.toString().toIntOrNull() ?: 2
            sendCommand("\"cmd\":\"autotx\",\"enabled\":$isAutoTx,\"interval\":$interval,\"packets\":$packets")
        }
        binding.btnSniffStart.setOnClickListener { sendCommand("\"cmd\":\"sniff_start\"") }
        binding.btnSniffStop.setOnClickListener { sendCommand("\"cmd\":\"sniff_stop\"") }
        binding.btnSniffApply.setOnClickListener { sendCommand("\"cmd\":\"sniff_apply\"") }
        binding.btnSaveFreq.setOnClickListener {
            val freq = if (binding.rb433.isChecked) 433 else 315
            sendCommand("\"cmd\":\"settings\",\"freq\":$freq")
        }
        binding.btnActivate.setOnClickListener {
            val key = binding.etLicense.text.toString().trim()
            if (key.length == 8) {
                sendCommand("\"cmd\":\"license\",\"key\":\"$key\"")
            } else {
                log("Ключ должен быть 8 hex символов")
            }
        }
    }

    private fun sendSensor(idx: Int) {
        val h = sensorViews[idx]
        val id = h.etId.text.toString().trim()
        val pressure = h.etPressure.text.toString().toIntOrNull() ?: 230
        val temp = h.etTemperature.text.toString().toIntOrNull() ?: 20
        val enabled = h.cbEnabled.isChecked
        sendCommand("\"cmd\":\"sensor\",\"sensor\":$idx,\"id\":\"$id\",\"pressure\":$pressure,\"temp\":$temp,\"enabled\":$enabled")
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
        binding.btnConnect.isEnabled = false
        binding.tvConnectionState.text = "Сканирование..."
        binding.tvConnectionState.setTextColor(getColor(android.R.color.holo_blue_light))

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
        // Add bonded devices as fallback if scan found nothing
        if (foundDevices.isEmpty()) {
            val bonded = bluetoothAdapter?.bondedDevices ?: emptySet()
            bonded.filter {
                val n = it.name ?: ""
                n.contains("TPMS", true) || n.contains("NRF", true)
            }.forEach { foundDevices.add(it) }
        }
        val items = foundDevices.map { "${it.name ?: "Unknown"} (${it.address})" }.toTypedArray()
        if (items.isEmpty()) {
            binding.tvConnectionState.text = "Устройства не найдены"
            binding.tvConnectionState.setTextColor(getColor(android.R.color.holo_red_light))
            return
        }
        AlertDialog.Builder(this)
            .setTitle("Выберите устройство")
            .setItems(items) { _, which ->
                selectedDevice = foundDevices[which]
                binding.btnConnect.isEnabled = true
                binding.btnConnect.text = "Подключить"
                binding.tvConnectionState.text = "Выбрано: ${foundDevices[which].name}"
                binding.tvConnectionState.setTextColor(getColor(android.R.color.holo_blue_light))
            }
            .setNegativeButton("Отмена", null)
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
        binding.tvConnectionState.text = "Подключение..."
        binding.tvConnectionState.setTextColor(getColor(android.R.color.holo_blue_light))
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
                    binding.tvConnectionState.text = "Подключено, поиск сервисов..."
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
                    log("BLE отключен")
                }
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
        // drain pending messages if any
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

    private fun sendCommandInternal(payload: String) {
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
            runOnUiThread { log("→ $msg") }
        } else {
            pendingChunks.clear()
            pendingChunks.addAll(bytes.toList())
            isSendingChunks = false
            Log.d(TAG, "TX chunked (${bytes.size} bytes in ${(bytes.size + chunkSize - 1) / chunkSize} chunks): $msg")
            runOnUiThread { log("→ $msg (${bytes.size}b)") }
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

    private fun sendCommand(payload: String) {
        if (!isConnected || bluetoothGatt == null) {
            log("Не подключено")
            return
        }
        if (!isNusReady) {
            pendingMessages.add(payload)
            log("В очереди: $payload")
            return
        }
        if (pendingMessages.isNotEmpty()) {
            pendingMessages.add(payload)
            return
        }
        sendCommandInternal(payload)
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
                h.etId.setText(s.optString("id", "00000000"))
                h.etPressure.setText(s.optInt("p", 2300).toString())
                h.etTemperature.setText(s.optInt("t", 20).toString())
                h.cbEnabled.isChecked = s.optInt("en", 0) == 1
            }
        }

        val freq = data.optInt("freq", 315)
        binding.rb315.isChecked = freq == 315
        binding.rb433.isChecked = freq == 433

        isAutoTx = data.optInt("tx_en", 0) == 1
        binding.btnAutoTx.text = if (isAutoTx) "Стоп" else "Старт"
        binding.etInterval.setText(data.optInt("tx_int", 300).toString())
        binding.etPackets.setText(data.optInt("tx_pkt", 2).toString())

        val licensed = data.optInt("license", 0) == 1
        binding.tvLicense.text = if (licensed) "Лицензия активирована" else "Без лицензии"
        binding.tvLicense.setTextColor(
            if (licensed) getColor(android.R.color.holo_green_light)
            else getColor(android.R.color.holo_orange_light)
        )

        val mv = data.optInt("batt_mv", 0)
        val pct = data.optInt("batt_pct", 0)
        binding.tvBattery.text = "Батарея: ${mv}mV (${pct}%)"
        binding.tvBattery.setTextColor(
            if (pct < 10) getColor(android.R.color.holo_red_light)
            else if (pct < 30) getColor(android.R.color.holo_orange_light)
            else getColor(android.R.color.holo_green_light)
        )

        val sniffActive = data.optInt("sniff", 0) == 1
        val disc = data.optJSONArray("disc")
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
        log(sb.toString().trimEnd())
    }

    private fun updateConnectionState() {
        if (isConnected) {
            binding.tvConnectionState.text = "Подключено: ${selectedDevice?.name ?: selectedDevice?.address}"
            binding.tvConnectionState.setTextColor(getColor(android.R.color.holo_green_light))
            binding.btnConnect.text = "Отключить"
        } else {
            binding.tvConnectionState.text = "Отключено"
            binding.tvConnectionState.setTextColor(getColor(android.R.color.holo_red_light))
            binding.btnConnect.text = "Подключить"
            binding.btnConnect.isEnabled = selectedDevice != null
        }
    }

    private fun log(msg: String) {
        val text = binding.tvLog
        text.append("$msg\n")
        (text.parent as? ScrollView)?.post {
            (text.parent as ScrollView).fullScroll(View.FOCUS_DOWN)
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
        val tvPosition: TextView = view.findViewById(R.id.tvPosition)
        val etId: EditText = view.findViewById(R.id.etSensorId)
        val etPressure: EditText = view.findViewById(R.id.etPressure)
        val etTemperature: EditText = view.findViewById(R.id.etTemperature)
        val cbEnabled: CheckBox = view.findViewById(R.id.cbEnabled)
        val btnSave: Button = view.findViewById(R.id.btnSaveSensor)
        val btnTx: Button = view.findViewById(R.id.btnTxSensor)
    }
}
