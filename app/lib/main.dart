import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_localizations/flutter_localizations.dart';

import 'deep_link.dart';
import 'l10n.dart';
import 'pages/home_page.dart';
import 'pages/session_page.dart';
import 'session_window.dart';
import 'theme.dart';

void main(List<String> args) {
  WidgetsFlutterBinding.ensureInitialized();
  AppLanguage.load();
  runApp(
    ChastApp(
      initialConnection: parseLaunchArguments(args),
      sessionWindow: SessionWindowRequest.fromArguments(args),
    ),
  );
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

class ChastApp extends StatelessWidget {
  const ChastApp({super.key, this.initialConnection, this.sessionWindow});

  final DeepLinkRequest? initialConnection;

  /// Non-null when this engine was started for [SessionWindowRequest.open];
  /// the window shows that one session instead of the device list.
  final SessionWindowRequest? sessionWindow;

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
          home: sessionWindow != null
              ? SessionPage(
                  target: sessionWindow!.target,
                  account: sessionWindow!.account,
                  settings: sessionWindow!.settings,
                  fileTransferOnly: sessionWindow!.filesOnly,
                  sessionMode: sessionWindow!.sessionMode,
                  rdpPort: sessionWindow!.rdpPort,
                  windowSession: true,
                  initialDisplay: sessionWindow!.display,
                )
              : HomePage(
                  initialTarget: initialConnection?.target,
                  initialFileTransferOnly:
                      initialConnection?.fileTransferOnly ?? false,
                ),
        );
      },
    );
  }
}
