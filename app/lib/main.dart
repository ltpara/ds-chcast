import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_localizations/flutter_localizations.dart';

import 'l10n.dart';
import 'pages/home_page.dart';
import 'theme.dart';

void main(List<String> args) {
  WidgetsFlutterBinding.ensureInitialized();
  AppLanguage.load();
  runApp(ChastApp(connectTarget: _connectTarget(args)));
  _markUpdateHealthy();
}

/// The update helper keeps the previous executable until this marker exists.
/// Writing it after the first frame means a release that crashes during startup
/// is rolled back instead of leaving the viewer unusable.
void _markUpdateHealthy() {
  final String path =
      (Platform.environment['CHAST_UPDATE_HEALTH_FILE'] ?? '').trim();
  if (path.isEmpty) {
    return;
  }
  WidgetsBinding.instance.addPostFrameCallback((_) {
    try {
      File(path).writeAsStringSync('healthy\n');
    } catch (_) {
      // A marker we cannot write only disables the automatic rollback; it must
      // never prevent the viewer from starting.
    }
  });
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
    return ValueListenableBuilder<String>(
      valueListenable: AppLanguage.current,
      builder: (BuildContext context, String language, Widget? child) {
        return MaterialApp(
          title: 'Chast',
          debugShowCheckedModeBanner: false,
          locale: Locale(language),
          supportedLocales: const <Locale>[Locale('zh', 'CN'), Locale('en')],
          localizationsDelegates:
              const <LocalizationsDelegate<Object>>[
                GlobalMaterialLocalizations.delegate,
                GlobalWidgetsLocalizations.delegate,
                GlobalCupertinoLocalizations.delegate,
              ],
          theme: buildChastTheme(Brightness.light),
          darkTheme: buildChastTheme(Brightness.dark),
          themeMode: ThemeMode.system,
          home: HomePage(initialTarget: connectTarget),
        );
      },
    );
  }
}
