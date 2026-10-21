import 'package:flutter/material.dart';

import 'pages/home_page.dart';
import 'theme.dart';

void main(List<String> args) {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(ChastApp(connectTarget: _connectTarget(args)));
}

String? _connectTarget(List<String> args) {
  for (int i = 0; i < args.length; ++i) {
    final String arg = args[i];
    if (arg == '--connect' && i + 1 < args.length) {
      return args[i + 1];
    }
    if (arg.startsWith('--connect=')) {
      return arg.substring('--connect='.length);
    }
  }
  return null;
}

class ChastApp extends StatelessWidget {
  const ChastApp({super.key, this.connectTarget});

  final String? connectTarget;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Chast',
      debugShowCheckedModeBanner: false,
      theme: buildChastTheme(Brightness.light),
      darkTheme: buildChastTheme(Brightness.dark),
      themeMode: ThemeMode.system,
      home: HomePage(initialTarget: connectTarget),
    );
  }
}
