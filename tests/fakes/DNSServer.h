#pragma once

#include "WiFi.h"

class DNSServer {
 public:
  bool running = false;
  unsigned processed = 0;
  bool start(unsigned, const char*, FakeAddress) { running = true; return true; }
  void stop() { running = false; }
  void processNextRequest() { ++processed; }
};
