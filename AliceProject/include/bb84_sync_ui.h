#pragma once
// Colorful, low-flicker BB84/WiFi SYNC dashboard inspired by the user's
// previous bb84_common.h display. Compatible with current V3 control protocol.
// No network, GPIO, servo, ADC, or laser code lives in this view.
#include "bb84_shared.h"

namespace demo {
class SyncDashboard {
 public:
  explicit SyncDashboard(Adafruit_ST7789& lcd, bool aliceMode)
      : lcd_(lcd), isAlice_(aliceMode) {}

  void invalidate() { initialized_ = false; }

  void update(char basis, int bit, int angle, unsigned slot, unsigned total,
              unsigned sift, unsigned errors, const String& metric,
              const String& status, bool wifiLinked, bool laserOn) {
    if (!initialized_) build();
    if (basis != oldBasis_) {
      oldBasis_ = basis;
      drawBasis(basis);
    }
    if (bit != oldBit_) {
      oldBit_ = bit;
      drawBit(bit);
    }
    if (slot != oldSlot_ || total != oldTotal_) {
      oldSlot_ = slot; oldTotal_ = total;
      lcd_.fillRect(55, 141, 104, 18, PANEL);
      lcd_.setTextSize(2); lcd_.setTextColor(CYAN);
      lcd_.setCursor(56, 142);
      char buf[17];
      snprintf(buf, sizeof(buf), "%03u/%03u", slot, total);
      lcd_.print(buf);
    }
    if (sift != oldSift_) {
      oldSift_ = sift;
      lcd_.fillRect(54, 163, 74, 17, PANEL);
      lcd_.setTextSize(2); lcd_.setTextColor(MINT);
      lcd_.setCursor(57, 163); lcd_.print(sift);
    }
    if (errors != oldErrors_) {
      oldErrors_ = errors;
      lcd_.fillRect(179, 163, 47, 17, PANEL);
      lcd_.setTextSize(2); lcd_.setTextColor(errors ? GOLD : MINT);
      lcd_.setCursor(182, 163); lcd_.print(errors);
    }
    if (metric != oldMetric_) {
      oldMetric_ = metric;
      lcd_.fillRect(12, 190, 214, 15, PANEL);
      lcd_.setTextSize(1); lcd_.setTextColor(WHITE);
      lcd_.setCursor(13, 193); lcd_.print(metric.substring(0, 35));
    }
    if (angle != oldAngle_ || status != oldStatus_ || laserOn != oldLaser_) {
      oldAngle_ = angle; oldStatus_ = status; oldLaser_ = laserOn;
      lcd_.fillRect(11, 214, 218, 20, PANEL);
      lcd_.setTextSize(1);
      lcd_.setTextColor(GOLD); lcd_.setCursor(13, 218);
      lcd_.print("HWP "); lcd_.print(angle); lcd_.print("  ");
      lcd_.setTextColor(laserOn ? RED : CYAN);
      lcd_.print(status.substring(0, 19));
    }
    if ((wifiLinked ? 1 : 0) != oldWifi_) {
      oldWifi_ = wifiLinked ? 1 : 0;
      lcd_.fillRect(166, 20, 62, 12, PANEL);
      lcd_.setTextSize(1);
      lcd_.setTextColor(wifiLinked ? GREEN : GOLD);
      lcd_.setCursor(168, 22); lcd_.print(wifiLinked ? "LINK OK" : "WAIT...");
    }
  }

 private:
  Adafruit_ST7789& lcd_;
  bool isAlice_;
  bool initialized_ = false;
  char oldBasis_ = '\0';
  int oldBit_ = -99, oldAngle_ = -999, oldWifi_ = -1;
  bool oldLaser_ = false;
  unsigned oldSlot_ = 65535, oldTotal_ = 65535, oldSift_ = 65535, oldErrors_ = 65535;
  String oldMetric_ = "\001", oldStatus_ = "\001";

  void build() {
    initialized_ = true;
    oldBasis_ = '\0'; oldBit_ = -99; oldAngle_ = -999; oldWifi_ = -1;
    oldSlot_ = oldTotal_ = oldSift_ = oldErrors_ = 65535;
    oldMetric_ = oldStatus_ = "\001"; oldLaser_ = false;
    lcd_.fillScreen(BG);
    lcd_.setTextWrap(false);
    lcd_.fillRoundRect(5, 5, 230, 29, 5, PANEL);
    lcd_.fillRect(6, 8, 4, 22, CYAN);
    lcd_.setTextSize(2); lcd_.setTextColor(WHITE);
    lcd_.setCursor(15, 10); lcd_.print(isAlice_ ? "ALICE" : "BOB");
    lcd_.setTextSize(1); lcd_.setTextColor(CYAN);
    lcd_.setCursor(99, 10); lcd_.print(isAlice_ ? "BB84 / TX" : "BB84 / RX");
    lcd_.setTextColor(MUTED); lcd_.setCursor(99, 22); lcd_.print("WiFi SYNC");

    lcd_.fillRoundRect(7, 40, 109, 94, 7, PANEL);
    lcd_.fillRoundRect(124, 40, 109, 94, 7, PANEL);
    lcd_.setTextSize(1); lcd_.setTextColor(MUTED);
    lcd_.setCursor(15, 47); lcd_.print(isAlice_ ? "TX BASIS" : "BOB BASIS");
    lcd_.setCursor(134, 47); lcd_.print(isAlice_ ? "TX BIT" : "RX BIT");

    lcd_.fillRoundRect(7, 138, 226, 44, 5, PANEL);
    lcd_.setTextSize(1); lcd_.setTextColor(MUTED);
    lcd_.setCursor(14, 145); lcd_.print("SLOT");
    lcd_.setCursor(14, 168); lcd_.print(isAlice_ ? "SIFT" : "SIFT");
    lcd_.setCursor(147, 168); lcd_.print("ERR");
    lcd_.drawFastHLine(13, 161, 214, BG);

    lcd_.fillRoundRect(7, 186, 226, 21, 4, PANEL);
    lcd_.fillRoundRect(7, 211, 226, 25, 4, PANEL);
  }

  void drawBasis(char basis) {
    lcd_.fillRect(20, 60, 91, 71, PANEL);
    const uint16_t color = basis=='+' ? CYAN : uint16_t(0xFA5F);
    if (basis=='+') {
      lcd_.fillRoundRect(53, 68, 14, 58, 5, color);
      lcd_.fillRoundRect(31, 90, 59, 14, 5, color);
    } else if (basis=='X') {
      for (int i=-3; i<=3; ++i) {
        lcd_.drawLine(35+i, 70, 93+i, 124, color);
        lcd_.drawLine(93+i, 70, 35+i, 124, color);
      }
    } else {
      lcd_.setTextSize(6); lcd_.setTextColor(MUTED);
      lcd_.setCursor(42, 71); lcd_.print("?");
    }
  }

  void drawBit(int bit) {
    lcd_.fillRect(139, 60, 89, 71, PANEL);
    if (bit<0) {
      lcd_.setTextSize(7); lcd_.setTextColor(MUTED);
      lcd_.setCursor(156, 69); lcd_.print("?");
    } else if (bit==0) {
      lcd_.fillRoundRect(153, 67, 51, 61, 17, MINT);
      lcd_.fillRoundRect(166, 80, 25, 35, 9, PANEL);
    } else {
      lcd_.fillRoundRect(177, 67, 14, 61, 4, GOLD);
      lcd_.fillRoundRect(159, 119, 51, 9, 3, GOLD);
      lcd_.drawLine(163, 87, 182, 70, GOLD);
    }
  }
};
} // namespace demo
