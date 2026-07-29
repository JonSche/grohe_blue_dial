#include "app/app.hpp"

extern "C" void app_main() {
  static app::App app;
  app.Run();
}
