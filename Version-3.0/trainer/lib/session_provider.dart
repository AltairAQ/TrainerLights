// lib/services/session_provider.dart
// ============================================================
// Holds live session state. Listens to BtService message stream
// and updates stats, node list, and persists to DB on stop.
// ============================================================
import 'dart:async';
import 'package:flutter/foundation.dart';
import 'package:intl/intl.dart';
import 'bt_service.dart';
import 'database_service.dart';
import '../models/db_models.dart';

class NodeInfo {
  final int    nodeId;
  final String ip;
  final String mac;
  bool         isLive;
  NodeInfo({required this.nodeId, required this.ip, required this.mac, this.isLive = true});
}

class SessionProvider extends ChangeNotifier {
  final BtService bt;
  StreamSubscription? _sub;

  SessionProvider(this.bt) {
    _sub = bt.messages.listen(_onMessage);
  }

  // ---- Live stats ----
  int    hits         = 0;
  int    misses       = 0;
  int    avgMs        = 0;
  int    bestMs       = 0;
  int    worstMs      = 0;
  bool   isTesting    = false;
  List<NodeInfo> nodes = [];

  // ---- Current session tracking ----
  int?   _currentSessionId;
  int?   _currentAthleteId;
  String _sessionMode = 'random';
  String _startedAt   = '';

  // Response time series for chart (hits only)
  final List<double> responseSeries = [];

  // ---- Settings (sent to ESP32) ----
  String mode           = 'random';
  int    minDelay       = 4000;
  int    maxDelay       = 4000;
  int    minTimeout     = 1000;
  int    maxTimeout     = 2000;
  int    minDetection   = 2;
  int    maxDetection   = 40;

  void _onMessage(Map<String, dynamic> msg) {
    final type = msg['type'] as String?;
    if (type == null) return;

    if (type == 'stats') {
      hits    = msg['test_score']        ?? 0;
      misses  = msg['test_errors']       ?? 0;
      avgMs   = msg['avg_response_time'] ?? 0;
      bestMs  = msg['min_response_time'] ?? 0;
      worstMs = msg['max_response_time'] ?? 0;
      final wasTesting = isTesting;
      isTesting = msg['is_testing'] ?? false;

      // Persist individual hit to DB
      if (_currentSessionId != null && wasTesting) {
        final now = DateFormat('yyyy-MM-dd HH:mm:ss').format(DateTime.now());
        // We can only infer the latest hit/miss from delta; simplest approach:
        // We push a Hit record every time stats update with a non-zero avg
        // Full per-hit logging would require the server to emit per-hit events.
        // Mark this as a best-effort record.
        if (avgMs > 0) {
          responseSeries.add(avgMs.toDouble());
        }
      }
      notifyListeners();
    }

    else if (type == 'sensor_list') {
      final List<dynamic> list = msg['sensors'] ?? [];
      final incoming = list.map((s) => NodeInfo(
        nodeId: s['node_id'] ?? 0,
        ip:     s['ip']      ?? '',
        mac:    s['mac']     ?? '',
        isLive: true,
      )).toList();

      // Mark drops
      for (var existing in nodes) {
        final still = incoming.any((n) => n.mac == existing.mac);
        existing.isLive = still;
      }
      // Add new arrivals
      for (var n in incoming) {
        if (!nodes.any((e) => e.mac == n.mac)) nodes.add(n);
      }
      notifyListeners();
    }
  }

  // ---- Session control ----

  Future<void> startSession(int athleteId) async {
    _currentAthleteId = athleteId;
    _sessionMode      = mode;
    _startedAt        = DateFormat('yyyy-MM-dd HH:mm:ss').format(DateTime.now());
    responseSeries.clear();
    hits = misses = avgMs = bestMs = worstMs = 0;

    final sessionId = await DatabaseService.insertSession(Session(
      athleteId:        athleteId,
      startedAt:        _startedAt,
      totalHits:        0,
      totalMisses:      0,
      avgResponseMs:    0,
      bestResponseMs:   0,
      worstResponseMs:  0,
      mode:             mode,
    ));
    _currentSessionId = sessionId;
    bt.send({'type': 'start_test'});
    notifyListeners();
  }

  Future<void> stopSession() async {
    bt.send({'type': 'stop_test'});
    if (_currentSessionId != null) {
      await DatabaseService.updateSession(Session(
        id:               _currentSessionId,
        athleteId:        _currentAthleteId ?? 0,
        startedAt:        _startedAt,
        totalHits:        hits,
        totalMisses:      misses,
        avgResponseMs:    avgMs,
        bestResponseMs:   bestMs,
        worstResponseMs:  worstMs,
        mode:             _sessionMode,
      ));
    }
    _currentSessionId = null;
    isTesting         = false;
    notifyListeners();
  }

  void clearStats() {
    bt.send({'type': 'clear_stats'});
    responseSeries.clear();
    hits = misses = avgMs = bestMs = worstMs = 0;
    notifyListeners();
  }

  void requestNodeList() => bt.send({'type': 'list_sensors'});
  void blinkAll()        => bt.send({'type': 'blink_all'});

  void removeNode(String mac) {
    bt.send({'type': 'remove_node', 'mac': mac});
    nodes.removeWhere((n) => n.mac == mac);
    notifyListeners();
  }

  void clearNodes() {
    bt.send({'type': 'clear_nodes'});
    nodes.clear();
    notifyListeners();
  }

  void pushConfig() {
    bt.send({
      'type':                 'config',
      'tmode':                mode,
      'min_delay':            minDelay,
      'max_delay':            maxDelay,
      'mim_timeout':          minTimeout,
      'max_timeout':          maxTimeout,
      'min_detection_range':  minDetection,
      'max_detection_range':  maxDetection,
    });
  }

  @override
  void dispose() {
    _sub?.cancel();
    super.dispose();
  }
}
