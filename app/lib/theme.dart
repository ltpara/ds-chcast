import 'package:flutter/material.dart';

const Color kAccent = Color(0xFF0071FF);
const Color kAccentBright = Color(0xFF2C8CFF);
const Color kCanvas = Color(0xFF212121);
const Color kHomeLight = Color(0xFFEFEFF2);
const Color kHomeDark = Color(0xFF18191E);
const Color kPanelDark = Color(0xFF24252B);
const Color kToolbarDark = Color(0xFF121212);

ThemeData buildChastTheme(Brightness brightness) {
  final bool dark = brightness == Brightness.dark;
  final ColorScheme scheme = ColorScheme(
    brightness: brightness,
    primary: kAccent,
    onPrimary: Colors.white,
    secondary: kAccentBright,
    onSecondary: Colors.white,
    error: const Color(0xFFE53935),
    onError: Colors.white,
    surface: dark ? kPanelDark : Colors.white,
    onSurface: dark ? Colors.white : const Color(0xFF1E2228),
  );
  return ThemeData(
    useMaterial3: true,
    brightness: brightness,
    colorScheme: scheme,
    scaffoldBackgroundColor: dark ? kHomeDark : kHomeLight,
    fontFamily: 'Segoe UI',
    visualDensity: VisualDensity.standard,
    dividerColor: dark ? Colors.white12 : const Color(0xFFE2E4E9),
    textSelectionTheme: TextSelectionThemeData(
      cursorColor: kAccent,
      selectionColor: kAccent.withValues(alpha: 0.25),
      selectionHandleColor: kAccent,
    ),
  );
}

BoxDecoration panelDecoration(BuildContext context, {double radius = 13}) {
  final ThemeData theme = Theme.of(context);
  final bool dark = theme.brightness == Brightness.dark;
  return BoxDecoration(
    color: theme.colorScheme.surface,
    borderRadius: BorderRadius.circular(radius),
    boxShadow: dark
        ? null
        : const <BoxShadow>[
            BoxShadow(
              color: Color(0x0F1A2233),
              blurRadius: 24,
              offset: Offset(0, 6),
            ),
          ],
  );
}

TextStyle mutedStyle(BuildContext context, {double size = 12}) {
  final bool dark = Theme.of(context).brightness == Brightness.dark;
  return TextStyle(
    fontSize: size,
    color: dark ? Colors.white54 : const Color(0xFF7C8390),
    height: 1.35,
  );
}
