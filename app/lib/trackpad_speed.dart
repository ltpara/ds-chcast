const int kMinTrackpadSpeedPercent = 10;
const int kDefaultTrackpadSpeedPercent = 100;
const int kMaxTrackpadSpeedPercent = 1000;

int normalizeTrackpadSpeedPercent(Object? value) =>
    value is int &&
        value >= kMinTrackpadSpeedPercent &&
        value <= kMaxTrackpadSpeedPercent
    ? value
    : kDefaultTrackpadSpeedPercent;
