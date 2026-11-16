#pragma once

#include "WiFi.h"
#include <functional>
#include <map>
#include <utility>

enum HTTPMethod { HTTP_GET, HTTP_POST };

class WebServer {
 public:
  explicit WebServer(int) {}
  std::map<std::pair<std::string, HTTPMethod>, std::function<void()>> routes;
  std::function<void()> notFound;
  std::map<std::string, String> arguments;
  std::map<std::string, String> headers;
  int status = 0;
  String body;
  bool running = false;
  unsigned handled = 0;

  void on(const char* path, HTTPMethod method, std::function<void()> handler) {
    routes[{path, method}] = handler;
  }
  void onNotFound(std::function<void()> handler) { notFound = handler; }
  void begin() { running = true; }
  void stop() { running = false; }
  void handleClient() { ++handled; }
  bool hasArg(const String& name) const { return arguments.count(name) != 0; }
  String arg(const String& name) const { return arguments.at(name); }
  void sendHeader(const char* name, const String& value, bool = false) {
    headers[name] = value;
  }
  void send(int code, const char*, const String& content) {
    status = code;
    body = content;
  }
  void request(const char* path, HTTPMethod method) {
    status = 0;
    body.clear();
    headers.clear();
    auto route = routes.find({path, method});
    if (route == routes.end()) notFound();
    else route->second();
  }
};
