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
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                runOnUiThread {
                    binding.tvConnectionState.text = "Подключено, поиск сервисов..."
                    log("BLE подключен")
                }
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
                    ContextCompat.checkSelfPermission(this@MainActivity, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED
                ) {
                    gatt?.discoverServices()
                } else if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
                    gatt?.discoverServices()
                }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                rxCharacteristic = null
                isNusReady = false
                pendingMessages.clear()
                rxBuffer.clear()
                runOnUiThread {
                    isConnected = false
                    updateConnectionState()
                    log("BLE отключен")
                }
            }
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

            gatt.setCharacteristicNotification(txChar, true)
            val descriptor = txChar.getDescriptor(CCCD_UUID)
            if (descriptor != null) {
                descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                gatt.writeDescriptor(descriptor)
            } else {
                onNusReady()
            }

            runOnUiThread {
                isConnected = true
                updateConnectionState()
                log("NUS сервис найден")
            }
        }

        override fun onCharacteristicChanged(gatt: BluetoothGatt?, characteristic: BluetoothGattCharacteristic?) {
            super.onCharacteristicChanged(gatt, characteristic)
            if (characteristic?.uuid == NUS_TX_UUID) {
                val value = characteristic.getStringValue(0) ?: ""
                Log.d(TAG, "RX fragment: $value")
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
                onNusReady()
            } else {
                runOnUiThread { log("Ошибка включения уведомлений: $status") }
            }
        }

        override fun onCharacteristicWrite(gatt: BluetoothGatt?, characteristic: BluetoothGattCharacteristic?, status: Int) {
            super.onCharacteristicWrite(gatt, characteristic, status)
            Log.d(TAG, "onCharacteristicWrite status=$status uuid=${characteristic?.uuid}")
            if (status != BluetoothGatt.GATT_SUCCESS) {
                runOnUiThread { log("Ошибка записи: $status") }
            } else {
                sendNextPending()
            }
        }
    }

    private fun onNusReady() {
        isNusReady = true
        runOnUiThread { log("NUS готов к передаче") }
        sendCommand("\"cmd\":\"status\"")
        // drain pending messages if any
        while (pendingMessages.isNotEmpty()) {
            sendCommand(pendingMessages.removeAt(0).removeSurrounding("{", "}"))
        }
    }

    private fun sendNextPending() {
        val next = pendingMessages.removeAt(0)
        sendCommandInternal(next)
    }

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
        rx.value = msg.toByteArray(Charsets.UTF_8)
        val ok = bluetoothGatt?.writeCharacteristic(rx) ?: false
        Log.d(TAG, "TX: $msg ok=$ok")
        runOnUiThread { log("→ $msg") }
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
                h.etPressure.setText(s.optInt("pressure", 230).toString())
                h.etTemperature.setText(s.optInt("temp", 20).toString())
                h.cbEnabled.isChecked = s.optBoolean("enabled", true)
            }
        }

        val freq = data.optInt("freq", 315)
        binding.rb315.isChecked = freq == 315
        binding.rb433.isChecked = freq == 433

        val autoTx = data.optJSONObject("autoTx")
        autoTx?.let {
            isAutoTx = it.optBoolean("enabled", false)
            binding.btnAutoTx.text = if (isAutoTx) "Стоп" else "Старт"
            binding.etInterval.setText(it.optInt("interval", 300).toString())
            binding.etPackets.setText(it.optInt("packets", 2).toString())
        }

        val license = data.optJSONObject("license")
        license?.let {
            val licensed = it.optBoolean("licensed", false)
            val trialSec = it.optInt("trialSec", 0)
            val remain = 86400 - trialSec
            binding.tvLicense.text = if (licensed) {
                "Лицензия активирована"
            } else if (remain > 0) {
                val h = remain / 3600
                val m = (remain % 3600) / 60
                "Пробный период: ${h}ч ${m}мин"
            } else {
                "Пробный период истек"
            }
            binding.tvLicense.setTextColor(
                if (licensed) getColor(android.R.color.holo_green_light)
                else if (remain > 0) getColor(android.R.color.holo_orange_light)
                else getColor(android.R.color.holo_red_light)
            )
        }

        val battery = data.optJSONObject("battery")
        battery?.let {
            val mv = it.optInt("mv", 0)
            val pct = it.optInt("pct", 0)
            binding.tvBattery.text = "Батарея: ${mv}mV (${pct}%)"
            when {
                it.optBoolean("critical", false) -> binding.tvBattery.setTextColor(getColor(android.R.color.holo_red_light))
                it.optBoolean("low", false) -> binding.tvBattery.setTextColor(getColor(android.R.color.holo_orange_light))
                else -> binding.tvBattery.setTextColor(getColor(android.R.color.holo_green_light))
            }
        }

        val sniffer = data.optJSONObject("sniffer")
        sniffer?.let {
            val active = it.optBoolean("active", false)
            val count = it.optInt("count", 0)
            val arr = it.optJSONArray("sensors")
            val sb = StringBuilder()
            sb.appendLine("Сниффер: ${if (active) "активен" else "выкл"}, найдено: $count")
            arr?.let { a ->
                for (i in 0 until a.length()) {
                    val s = a.getJSONObject(i)
                    sb.appendLine("ID:${s.optString("id")} P:${s.optInt("p")} T:${s.optInt("t")}")
                }
            }
            binding.tvSniffResults.text = sb.toString()
        }
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
        if (requestCode == REQUEST_PERMISSIONS && grantResults.any { it != PackageManager.PERMISSION_GRANTED }) {
            Toast.makeText(this, "Без разрешений Bluetooth работать не будет", Toast.LENGTH_LONG).show()
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
