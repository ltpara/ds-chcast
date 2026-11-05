import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_localizations/flutter_localizations.dart';

import 'deep_link.dart';
import 'l10n.dart';
import 'pages/home_page.dart';
import 'pages/session_page.dart';
import 'session_window.dart';
import 'theme.dart';
import 'tunnel_agent.dart';

void main(List<String> args) {
  WidgetsFlutterBinding.ensureInitialized();
  AppLanguage.load();
  WidgetsBinding.instance.addPostFrameCallback((_) {
    unawaited(TunnelAgent.confirmUpdate());
  });
  runApp(ChastApp(initialConnection: parseLaunchArguments(args),
    sessionWindow: SessionWindowRequest.fromArguments(args)));
}

class ChastApp extends StatelessWidget {
  const ChastApp({super.key, this.initialConnection, this.sessionWindow});
  final SessionWindowRequest? sessionWindow;

  final DeepLinkRequest? initialConnection;

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
          home: sessionWindow != null ? SessionPage(
            target: sessionWindow!.target, account: sessionWindow!.account,
            settings: sessionWindow!.settings, fileTransferOnly: sessionWindow!.filesOnly,
            windowSession: true, initialDisplay: sessionWindow!.display,
          ) : HomePage(
            initialTarget: initialConnection?.target,
            initialFileTransferOnly:
                initialConnection?.fileTransferOnly ?? false,
          ),
        );
      },
    );
  }
}
