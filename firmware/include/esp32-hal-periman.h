// Stub for Arduino-ESP32 3.x peripheral manager header.
// The GFX library includes this unconditionally on ESP32 targets but never
// calls any periman functions in the code paths we use (QSPI, not SPI).
// This stub satisfies the include when building against Arduino-ESP32 2.x.
#pragma once
