enum ViewScrollStyle { scrollbar, auto, edge }

const defaultViewScrollStyle = ViewScrollStyle.scrollbar;
const minViewScrollEdge = 20;
const maxViewScrollEdge = 150;
const defaultViewScrollEdge = 100;

ViewScrollStyle parseViewScrollStyle(Object? raw) => switch (raw) {
  'auto' => ViewScrollStyle.auto,
  'edge' => ViewScrollStyle.edge,
  _ => defaultViewScrollStyle,
};

int normalizeViewScrollEdge(Object? raw) =>
    raw is int && raw >= minViewScrollEdge && raw <= maxViewScrollEdge
    ? raw
    : defaultViewScrollEdge;
