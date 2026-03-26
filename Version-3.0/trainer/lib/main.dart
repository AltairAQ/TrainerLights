// lib/main.dart
import 'package:flutter/material.dart';
import 'package:provider/provider.dart';
import 'services/bt_service.dart';
import 'services/session_provider.dart';
import 'screens/connect_screen.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(
    MultiProvider(
      providers: [
        ChangeNotifierProvider(create: (_) => BtService()),
        ChangeNotifierProxyProvider<BtService, SessionProvider>(
          create:  (ctx) => SessionProvider(ctx.read<BtService>()),
          update:  (ctx, bt, prev) => prev ?? SessionProvider(bt),
        ),
      ],
      child: const TrainerLightsApp(),
    ),
  );
}

class TrainerLightsApp extends StatelessWidget {
  const TrainerLightsApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'TrainerLights',
      debugShowCheckedModeBanner: false,
      theme:     _lightTheme(),
      darkTheme: _darkTheme(),
      themeMode: ThemeMode.system,   // follows device dark/light setting
      home: const ConnectScreen(),
    );
  }

  ThemeData _lightTheme() => ThemeData(
    useMaterial3:  true,
    colorSchemeSeed: const Color(0xFF007AFF),
    brightness:    Brightness.light,
    cardTheme:     CardTheme(
      elevation: 0,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
      color: Colors.white,
      shadowColor: Colors.black12,
    ),
    appBarTheme: const AppBarTheme(
      backgroundColor: Color(0xFF1D1D1F),
      foregroundColor: Colors.white,
      elevation: 0,
      centerTitle: false,
      titleTextStyle: TextStyle(
        fontSize: 20, fontWeight: FontWeight.w800, color: Colors.white),
    ),
  );

  ThemeData _darkTheme() => ThemeData(
    useMaterial3:  true,
    colorSchemeSeed: const Color(0xFF007AFF),
    brightness:    Brightness.dark,
    cardTheme:     CardTheme(
      elevation: 0,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(16)),
      color: const Color(0xFF1C1C1E),
    ),
    appBarTheme: const AppBarTheme(
      backgroundColor: Color(0xFF1D1D1F),
      foregroundColor: Colors.white,
      elevation: 0,
      centerTitle: false,
      titleTextStyle: TextStyle(
        fontSize: 20, fontWeight: FontWeight.w800, color: Colors.white),
    ),
    scaffoldBackgroundColor: const Color(0xFF000000),
  );
}
