// lib/services/bt_service.dart
// ============================================================
// Handles BT Classic SPP connection to the ESP32 server.
// Exposes a stream of parsed JSON maps to the rest of the app.
// ============================================================
import 'dart:async';
import 'dart:convert';
import 'package:flutter/foundation.dart';
import 'package:flutter_bluetooth_serial/flutter_bluetooth_serial.dart';

class BtService extends ChangeNotifier {
  BluetoothConnection? _connection;
  String _rxBuffer = '';

  bool get isConnected => _connection?.isConnected ?? false;

  // Stream controller — emits one Map per complete JSON message
  final _msgController = StreamController<Map<String, dynamic>>.broadcast();
  Stream<Map<String, dynamic>> get messages => _msgController.stream;

  // ---- Scan for paired devices named "TrainerLights" ----
  Future<BluetoothDevice?> findDevice() async {
    final devices = await FlutterBluetoothSerial.instance.getBondedDevices();
    try {
      return devices.firstWhere((d) => d.name == 'TrainerLights');
    } catch (_) {
      return null;
    }
  }

  // ---- Connect ----
  Future<bool> connect(BluetoothDevice device) async {
    try {
      _connection = await BluetoothConnection.toAddress(device.address);
      _connection!.input!.listen(_onData, onDone: _onDisconnect);
      notifyListeners();
      return true;
    } catch (e) {
      debugPrint('[BT] Connect error: $e');
      return false;
    }
  }

  // ---- Disconnect ----
  Future<void> disconnect() async {
    await _connection?.close();
    _connection = null;
    notifyListeners();
  }

  // ---- Incoming data — accumulate until newline ----
  void _onData(Uint8List data) {
    _rxBuffer += utf8.decode(data, allowMalformed: true);
    while (_rxBuffer.contains('\n')) {
      final idx  = _rxBuffer.indexOf('\n');
      final line = _rxBuffer.substring(0, idx).trim();
      _rxBuffer  = _rxBuffer.substring(idx + 1);
      if (line.isEmpty) continue;
      try {
        final map = json.decode(line) as Map<String, dynamic>;
        _msgController.add(map);
      } catch (e) {
        debugPrint('[BT] Bad JSON: $line');
      }
    }
  }

  void _onDisconnect() {
    _connection = null;
    notifyListeners();
  }

  // ---- Send a JSON message to the ESP32 ----
  void send(Map<String, dynamic> msg) {
    if (!isConnected) return;
    try {
      final bytes = utf8.encode('${json.encode(msg)}\n');
      _connection!.output.add(Uint8List.fromList(bytes));
    } catch (e) {
      debugPrint('[BT] Send error: $e');
    }
  }

  @override
  void dispose() {
    _msgController.close();
    _connection?.close();
    super.dispose();
  }
}
