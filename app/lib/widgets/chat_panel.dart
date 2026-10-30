import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../theme.dart';
import 'package:chast_viewer/l10n.dart';

/// One line of session chat, either sent by this viewer or received from the
/// host. The protocol carries plain UTF-8 text without a direction field; the
/// page tags each entry when it moves through one of the two queues.
class ChatEntry {
  const ChatEntry({required this.text, required this.mine, required this.at});

  final String text;
  final bool mine;
  final DateTime at;
}

/// The protocol's per-message ceiling. The native side rejects longer text,
/// so the panel checks before queueing and explains the limit inline.
const int kMaxChatBytes = 4096;

/// Modeless chat overlay for an active session. The page owns the entries and
/// the transport; the panel only validates, displays and collects input.
class ChatPanel extends StatefulWidget {
  const ChatPanel({
    super.key,
    required this.entries,
    required this.sent,
    required this.received,
    required this.onSend,
    required this.onClose,
    required this.inputFocusNode,
  });

  final List<ChatEntry> entries;
  final int sent;
  final int received;
  final Future<bool> Function(String text) onSend;
  final VoidCallback onClose;
  final FocusNode inputFocusNode;

  /// Null when [text] can be sent, otherwise a message for the inline hint.
  static String? validate(String text) {
    final String value = text.trim();
    if (value.isEmpty) {
      return tr('消息不能为空');
    }
    if (value.contains('\u0000')) {
      return tr('消息不能包含空字符');
    }
    final int bytes = utf8.encode(value).length;
    if (bytes > kMaxChatBytes) {
      return tr('消息过长：{bytes} / {kMaxChatBytes} 字节', <String, Object?>{'bytes': bytes, 'kMaxChatBytes': kMaxChatBytes});
    }
    return null;
  }

  @override
  State<ChatPanel> createState() => _ChatPanelState();
}

class _ChatPanelState extends State<ChatPanel> {
  final TextEditingController _input = TextEditingController();
  final ScrollController _scroll = ScrollController();
  bool _sending = false;
  String? _error;
  int _lastLength = 0;

  @override
  void initState() {
    super.initState();
    _lastLength = widget.entries.length;
    _scheduleScroll();
  }

  @override
  void didUpdateWidget(ChatPanel oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.entries.length != _lastLength) {
      _lastLength = widget.entries.length;
      _scheduleScroll();
    }
  }

  @override
  void dispose() {
    _input.dispose();
    _scroll.dispose();
    super.dispose();
  }

  void _scheduleScroll() {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || !_scroll.hasClients) {
        return;
      }
      _scroll.jumpTo(_scroll.position.maxScrollExtent);
    });
  }

  Future<void> _submit() async {
    if (_sending) {
      return;
    }
    final String text = _input.text.trim();
    final String? invalid = ChatPanel.validate(text);
    if (invalid != null) {
      setState(() => _error = invalid);
      return;
    }
    setState(() {
      _sending = true;
      _error = null;
    });
    bool queued = false;
    try {
      queued = await widget.onSend(text);
    } on PlatformException {
      queued = false;
    } on MissingPluginException {
      queued = false;
    } finally {
      if (mounted) {
        setState(() => _sending = false);
      }
    }
    if (!mounted) {
      return;
    }
    if (!queued) {
      setState(() => _error = tr('发送失败：会话未就绪或发送队列已满'));
      return;
    }
    _input.clear();
  }

  @override
  Widget build(BuildContext context) {
    return Material(
      color: kPanelDark,
      elevation: 10,
      borderRadius: BorderRadius.circular(9),
      child: Container(
        width: 330,
        height: 420,
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(9),
          border: Border.all(color: Colors.white24),
        ),
        child: Column(
          children: <Widget>[
            SizedBox(
              height: 38,
              child: Row(
                children: <Widget>[
                  const SizedBox(width: 10),
                  const Icon(Icons.forum_rounded, size: 16, color: kAccentBright),
                  const SizedBox(width: 7),
                  Text(
                    tr('会话聊天'),
                    style: TextStyle(
                      color: Colors.white,
                      fontSize: 12.5,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                  const SizedBox(width: 10),
                  Expanded(
                    child: Text(
                      tr('收 {p1} · 发 {p2}', <String, Object?>{'p1': widget.received, 'p2': widget.sent}),
                      textAlign: TextAlign.right,
                      style: const TextStyle(color: Colors.white38, fontSize: 10.5),
                    ),
                  ),
                  SizedBox(
                    width: 30,
                    height: 30,
                    child: IconButton(
                      padding: EdgeInsets.zero,
                      tooltip: tr('关闭聊天'),
                      icon: const Icon(Icons.close_rounded, size: 16),
                      color: Colors.white54,
                      onPressed: widget.onClose,
                    ),
                  ),
                  const SizedBox(width: 4),
                ],
              ),
            ),
            const Divider(height: 1, color: Colors.white12),
            Expanded(
              child: widget.entries.isEmpty
                  ? Center(
                      child: Text(
                        tr('还没有消息。发送一条试试。'),
                        style: TextStyle(color: Colors.white30, fontSize: 11),
                      ),
                    )
                  : ListView.builder(
                      controller: _scroll,
                      padding: const EdgeInsets.fromLTRB(10, 10, 10, 6),
                      itemCount: widget.entries.length,
                      itemBuilder: (BuildContext context, int index) {
                        return _ChatBubble(entry: widget.entries[index]);
                      },
                    ),
            ),
            if (_error != null)
              Padding(
                padding: const EdgeInsets.fromLTRB(12, 0, 12, 4),
                child: Align(
                  alignment: Alignment.centerLeft,
                  child: Text(
                    _error!,
                    style: const TextStyle(color: Colors.orangeAccent, fontSize: 10.5),
                  ),
                ),
              ),
            Container(
              padding: const EdgeInsets.fromLTRB(8, 6, 8, 8),
              decoration: const BoxDecoration(
                border: Border(top: BorderSide(color: Colors.white12)),
              ),
              child: Row(
                children: <Widget>[
                  Expanded(
                    child: TextField(
                      controller: _input,
                      focusNode: widget.inputFocusNode,
                      autofocus: false,
                      maxLines: 1,
                      textInputAction: TextInputAction.send,
                      style: const TextStyle(color: Colors.white, fontSize: 12),
                      cursorColor: kAccentBright,
                      decoration: InputDecoration(
                        isDense: true,
                        hintText: tr('输入消息，Enter 发送'),
                        hintStyle:
                            const TextStyle(color: Colors.white30, fontSize: 11.5),
                        filled: true,
                        fillColor: const Color(0xFF1B1C21),
                        contentPadding:
                            const EdgeInsets.symmetric(horizontal: 9, vertical: 9),
                        border: OutlineInputBorder(
                          borderRadius: BorderRadius.circular(6),
                          borderSide: BorderSide.none,
                        ),
                        focusedBorder: OutlineInputBorder(
                          borderRadius: BorderRadius.circular(6),
                          borderSide: const BorderSide(color: kAccent, width: 1),
                        ),
                      ),
                      onSubmitted: (_) => _submit(),
                      onChanged: (_) {
                        if (_error != null) {
                          setState(() => _error = null);
                        }
                      },
                    ),
                  ),
                  const SizedBox(width: 6),
                  SizedBox(
                    width: 34,
                    height: 34,
                    child: IconButton(
                      padding: EdgeInsets.zero,
                      tooltip: tr('发送'),
                      icon: const Icon(Icons.send_rounded, size: 17),
                      color: _sending ? Colors.white24 : kAccentBright,
                      onPressed: _sending ? null : _submit,
                    ),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _ChatBubble extends StatelessWidget {
  const _ChatBubble({required this.entry});

  final ChatEntry entry;

  static String _two(int value) => value.toString().padLeft(2, '0');

  @override
  Widget build(BuildContext context) {
    final String time =
        '${_two(entry.at.hour)}:${_two(entry.at.minute)}';
    return Padding(
      padding: const EdgeInsets.only(bottom: 8),
      child: Align(
        alignment: entry.mine ? Alignment.centerRight : Alignment.centerLeft,
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 250),
          child: Column(
            crossAxisAlignment:
                entry.mine ? CrossAxisAlignment.end : CrossAxisAlignment.start,
            children: <Widget>[
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 7),
                decoration: BoxDecoration(
                  color: entry.mine
                      ? kAccent.withValues(alpha: 0.82)
                      : const Color(0xFF303138),
                  borderRadius: BorderRadius.circular(7),
                ),
                child: SelectableText(
                  entry.text,
                  style: const TextStyle(color: Colors.white, fontSize: 12, height: 1.3),
                ),
              ),
              const SizedBox(height: 2),
              Text(
                tr('{p5} · {time}', <String, Object?>{'p5': entry.mine ? tr('我') : tr('主机'), 'time': time}),
                style: const TextStyle(color: Colors.white30, fontSize: 9.5),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
