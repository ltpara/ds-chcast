import 'package:flutter/material.dart';
import 'package:flutter_localizations/flutter_localizations.dart';

import 'l10n.dart';
import 'pages/home_page.dart';
import 'theme.dart';

void main(List<String> args) {
  WidgetsFlutterBinding.ensureInitialized();
  AppLanguage.load();
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
