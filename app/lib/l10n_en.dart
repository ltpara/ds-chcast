/// English renderings keyed by the Chinese source string used in the app.
///
/// Keys with placeholders use `{name}` exactly as the corresponding `tr(...)`
/// call in the UI does. A key that is absent falls back to the Chinese text,
/// so an incomplete dictionary degrades gracefully; the localization test
/// keeps the table in sync with the sources it can see.
library;

import 'l10n/en_core.dart';
import 'l10n/en_dialogs.dart';
import 'l10n/en_home.dart';
import 'l10n/en_session.dart';

const Map<String, String> kEnglishMessages = <String, String>{
  ...kEnglishHome,
  ...kEnglishSession,
  ...kEnglishDialogs,
  ...kEnglishCore,
};
